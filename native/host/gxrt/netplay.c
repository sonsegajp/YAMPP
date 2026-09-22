/* Netplay: lobby client, relay transport, synchronized menus and match rollback.
 *
 * Model: every peer runs the same deterministic recompiled game. A session is
 * a sequence of epochs, one per scene entry (character select, stage select,
 * match, results). Within an epoch the n-th HSD_PadRenewMasterStatus call is
 * frame n; each peer contributes its own pad for frame n + delay. Menus wait
 * for actual inputs; matches predict up to eight frames and restore/replay
 * complete simulation steps when late inputs disagree. The RNG is reseeded at
 * every epoch from the shared seed. Confirmed hashes detect divergence.
 *
 * Threads: the network thread owns the sockets and the lobby protocol; the
 * guest thread owns frame progression. Input rings are protected by a lock. */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include "platform_compat.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define closesocket close
#define WSAGetLastError() errno
#define WSAEWOULDBLOCK EWOULDBLOCK
#define SD_BOTH SHUT_RDWR
#define NP_SEND_FLAGS MSG_NOSIGNAL
#endif
#ifndef NP_SEND_FLAGS
#define NP_SEND_FLAGS 0
#endif
#include "abi_recompcore.h"
#include "recomp_funcs.h"
#include "../netplay_ui.h"
#include "../aurora_shim/aurora_shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rollback.h"
#include "timesync.h"
#include "costume_art.h"
#ifdef _WIN32
#undef X509_NAME
#undef X509_CERT_PAIR
#endif
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>

extern void func_8037750C(Context*);
extern int aurora_link_netplay_bind(void* ui);
extern void aurora_link_netplay_settings(char* server, unsigned server_cap, char* name, unsigned name_cap);
extern void aurora_link_netplay_save_settings(const char* server, const char* name);
extern void recomp_poll(Context*);
extern void frontend_drop_frame_backlog(void);
extern int aurora_link_widescreen(int set, int value);
extern int aurora_link_aspect_lock(int command);

#define NP_RING 1024
/* Every packet repeats the last few frames of input. Over UDP that is what
 * repairs a dropped datagram without waiting for a retransmission; over TCP
 * it costs a little bandwidth and covers a peer that asked for a resend. */
#define NP_REDUNDANT 8
/* A hiccup is not a disconnect. The match is held, visibly, and resumes the
 * moment input arrives again; only a link that stays dead this long ends it. */
#define NP_STALL_GRACE_MS 45000
/* Shown as "waiting for opponent" once a frame has waited this long. */
#define NP_STALL_NOTICE_MS 250
#define NP_PEER_TIMEOUT_MS 45000
/* Before the first frame there is no match to protect, so peers that never
 * reach each other are reported promptly instead of after the match grace. */
#define NP_ARM_TIMEOUT_MS 20000
/* A dropped lobby connection during a match is reconnected underneath the
 * players rather than ending the session. */
#define NP_RECONNECT_GRACE_MS 40000
#define NP_RECONNECT_BACKOFF_MS 1500
/* A peer that said goodbye may simply be reconnecting; wait before believing it. */
#define NP_BYE_GRACE_MS 8000
#define NP_PING_INTERVAL_MS 200
#define NP_SYNC_INTERVAL 30      /* frames between clock corrections */
#define NP_UDP_PROBE_MS 200
#define NP_UDP_QUIET_MS 1500     /* UDP considered lost; fall back to TCP */
#define NP_UDP_TOKEN 32
/* Hole punching. Both players send to each other's observed address at the
 * same time; whichever direction opens first carries the reply that proves
 * the other. Probes continue for the life of the session to hold the mapping
 * open, at a rate a NAT will not mistake for a flood. */
#define NP_PUNCH_MS 250
/* A direct path that has gone this long without a packet is abandoned and its
 * peer goes back through the relay, which is always still there. */
#define NP_DIRECT_QUIET_MS 2000
/* One public address as the server observed it, one private address the peer
 * reported: the second is what lets two players on the same network reach
 * each other without leaving it. */
#define NP_CANDIDATES 2
#define NP_MAGIC 0x4D4C4E50u   /* 'MLNP' */
#define NP_DEFAULT_PORT 7420
/* The public hostname is safe to distribute. Deployment-specific routing is
 * read from ignored local configuration and is never displayed in the UI. */
static const char* official_server(void) {
  static char configured[128];
  if (!configured[0]) {
    const char* override = getenv("MELEE_NETPLAY_OFFICIAL");
    snprintf(configured, sizeof configured, "%s", override && *override ? override : "mmodx.fun/melee/");
    if (!override || !*override) {
      FILE* file = fopen("config/online.xml", "rb");
      if (file) {
        char xml[4096]; size_t got = fread(xml, 1, sizeof xml - 1, file); xml[got] = 0; fclose(file);
        char* begin = strstr(xml, "<server>"), *end = begin ? strstr(begin + 8, "</server>") : NULL;
        if (begin && end && end > begin + 8 && (size_t)(end - begin - 8) < sizeof configured && !strstr(xml, "<!")) {
          begin += 8; size_t length = (size_t)(end - begin); int valid = 1;
          for (size_t i = 0; i < length; ++i) if ((unsigned char)begin[i] <= 32 || begin[i] == '<' || begin[i] == '>') valid = 0;
          if (valid) { memcpy(configured, begin, length); configured[length] = 0; }
        }
      }
    }
  }
  return configured;
}
static int is_official(const char* server) { return server && !strcmp(server, official_server()); }
#define PADLIB 0x804C1F78u     /* HSD_PadLibData: qnum u8, qread u8, qwrite u8, qcount u8, +8 queue */
#define RNG_SEED_PTR 0x804D5F94u
#define FN_GAME_RULES 0x8015CC34u
#define FN_PREFS 0x8015CC58u
#define FN_PLAYER_ENTITY 0x80034110u

enum { PKT_HELLO = 0, PKT_INPUT = 1, PKT_BYE = 2, PKT_REQUEST = 3, PKT_PING = 4, PKT_PONG = 5 };

#pragma pack(push, 1)
typedef struct NetInput { uint16_t buttons; int8_t sx, sy, cx, cy; uint8_t lt, rt; } NetInput;
typedef struct RelayHeader { uint32_t magic, session; uint16_t from, to; } RelayHeader;
/* `ack` is the sender's own simulated frame when the packet left, and
 * `advantage` is how far behind us they measure themselves to be. The pair is
 * what lets both sides agree on which one is running ahead. */
typedef struct InputHeader { uint8_t type, epoch, port, count; uint32_t last_frame, ack, hash_frame, hash; int8_t advantage; } InputHeader;
typedef struct PingPacket { uint8_t type, port; uint32_t tick; } PingPacket;
#pragma pack(pop)
typedef struct InputSlot { uint8_t valid; int epoch; uint32_t frame; NetInput in; } InputSlot;

/* Everything measured about one other player: how far apart the two clocks
 * have drifted, how long the link has been quiet, and whether they have said
 * goodbye (which is not yet the same as being gone). */
typedef struct NetPeer {
  TimeSync sync;
  uint32_t last_frame;        /* newest frame they have reported running */
  int advantage;              /* our measurement, sent back to them */
  int remote_advantage;       /* theirs, as received */
  DWORD ping_sent, last_rx;
  uint32_t ping_tick;
  int ping_outstanding;
  int gone; DWORD gone_at;    /* PKT_BYE seen; they may still be reconnecting */
  /* The direct path to this player. `token` is what their client expects to
   * see in front of a datagram we send it -- issued by the server, known only
   * to the two of us, and the only thing that makes a datagram from an
   * unknown address believable. */
  char token[NP_UDP_TOKEN + 1];
  struct sockaddr_in candidate[NP_CANDIDATES];
  int candidates;
  struct sockaddr_in direct_addr;
  int direct_ok;
  DWORD direct_rx, direct_probe;
} NetPeer;
static NetPeer np_peer[4];

typedef struct NetSend {
  struct NetSend* next;
  size_t length, offset;
  char data[];
} NetSend;
#define NP_SEND_LIMIT (1024u * 1024u)
static struct {
  CRITICAL_SECTION lock, send_lock;
  NetSend *send_head, *send_tail;
  size_t send_bytes;
  int send_failed;
  MeleeNetplayUi ui;
#ifdef _WIN32
  HANDLE thread;
#else
  pthread_t thread;
  int thread_valid;
#endif
  volatile LONG quit, lobby_open, active, armed, start_pending, guest_started, desynced;
  SOCKET tcp;
  int connecting, connected;
  SSL_CTX* ssl_ctx; SSL* ssl; int tls_active;
  /* Set when the address carries a path: the server then sits behind a reverse
   * proxy, so the connection opens with an HTTP upgrade before the protocol. */
  char http_host[128], http_path[96]; int upgrading, use_tls;
  char rx[65536]; unsigned rx_len;
  struct sockaddr_in server_addr;
  DWORD connect_started, last_ping, last_hello, last_rx;
  char pending_room[48]; int auto_mode, auto_host, auto_started;
  /* session */
  uint32_t session, seed; int delay, local_port, local_id;
  int port_ids[4];                 /* lobby id per guest port, 0 = unused */
  uint8_t peer_seen[4];
  int epoch; uint32_t frame; int scene_epoch_ready;
  InputSlot inputs[4][2][NP_RING];
  NetInput local_merge; int local_merge_valid;
  uint32_t hashes[256]; uint32_t hash_valid_from;
  uint32_t remote_hash_frame[4], remote_hash[4]; uint8_t remote_hash_epoch[4], remote_hash_new[4];
  uint8_t backup_rules[0x18], backup_prefs[0x20], backup_vs[0x140]; int rules_backed, vs_backed;
  DWORD stall_started;
  char script[4096]; int have_script;
  /* Unreliable side channel. Input is ordered by the frame numbers it carries,
   * never by the transport, so datagrams may arrive late, twice or not at all.
   * Losing one costs nothing: the next packet repeats it. Losing one inside a
   * TCP stream stalls every input behind it until the retransmission lands,
   * which is the stutter this channel exists to remove. */
  SOCKET udp; struct sockaddr_in udp_addr;
  int udp_offered, udp_ready; DWORD udp_last_rx, udp_last_probe;
  char udp_token[NP_UDP_TOKEN + 1];
  /* The direct path. One socket serves both routes: the relay answers from
   * its own address with no token in front, a player answers from theirs with
   * our token in front, and the two are never confused for each other.
   * `peer_token` is ours -- what we require on a datagram claiming to be from
   * a player. `local_udp` is this machine's own address on its network, which
   * the server passes to the other player as a second thing to try. */
  char peer_token[NP_UDP_TOKEN + 1];
  char local_udp[64];
  int direct_peers;
  /* Set once at startup. Off means every packet keeps going through the
   * server, which costs the extra hop and keeps this machine's address
   * between it and the server. */
  int allow_direct;
  /* Session continuity. An interruption holds the match; it does not end it. */
  volatile LONG interrupted;
  DWORD interrupt_since; char interrupt_why[96];
  int resuming; DWORD resume_since, reconnect_at;
  /* The relay sizes the automatic delay from the round trip we report to it,
   * so a fresh measurement is pushed as soon as it exists rather than on the
   * next heartbeat -- a room can be created and started in under a second. */
  int ping_reported;
  /* Clock agreement, applied by the scene loop. */
  int skip_frames;
} np;

static Rollback rollback;
static int rollback_match, rollback_running, rollback_step, rollback_replay;
static RbInput rollback_pads[4];
static uint32_t rollback_hash_sent;
static uint64_t rollback_clock, rollback_audio_origin, rollback_audio_clock;
static int rollback_service;
static unsigned rollback_progress;
/* Guest-thread only: asynchronous devices must not mutate an atomic simulation
 * step. Real controller sampling and presentation resume between live frames. */
int netplay_simulating(void) { return rollback_step; }
int netplay_replaying(void) { return rollback_replay; }
int netplay_devices_deferred(void) { return rollback_running && !rollback_service; }
int netplay_async_deferred(void) { return rollback_running; }
int netplay_audio_clock(uint64_t* clock) { if (!rollback_running) return 0; *clock = rollback_audio_clock; return 1; }
int netplay_clock(uint64_t* clock) { if (!rollback_running) return 0; *clock = rollback_clock; return 1; }
void netplay_progress(Context* ctx) {
  if (!rollback_step || rollback_service || !(ctx->msr & 0x8000u)) return;
  if ((++rollback_progress & 255u) != 0) return;
  /* Synchronous animation ARAM reads wait inside a simulation frame. Advance
   * their completion queue at deterministic guest progress, never wall time. */
  rollback_clock += 675u; ctx->timebase = rollback_clock;
  extern void arq_drain(Context*);
  rollback_service = 1; arq_drain(ctx); rollback_service = 0;
}


static uint32_t fnv(uint32_t h, uint32_t v) { for (int i = 0; i < 4; i++) { h ^= (v >> (i * 8)) & 0xFF; h *= 16777619u; } return h; }
static int valid_guest(uint32_t p, uint32_t size) { return p >= 0x80000000u && (uint64_t)p + size <= 0x81800000u; }
static void lock(void) { EnterCriticalSection(&np.lock); }
static void unlock(void) { LeaveCriticalSection(&np.lock); }
static void status(const char* text) { lock(); snprintf(np.ui.status, sizeof np.ui.status, "%s", text); unlock(); fprintf(stderr, "[netplay] %s\n", text); }
int netplay_post(int cmd, int arg, const char* text, const NetplayRules* rules);

/* Called by SDL text input and the guest's controller menu. All draft edits
 * and the final create command share the normal command lock. */
int netplay_room_name_input(int action, const char* text) {
  int accepted = 0;
  lock();
  if (action == NETPLAY_NAME_OPEN) {
    if (!np.ui.room_id && !np.active && !np.armed && !np.ui.mod_prompt) {
      np.ui.room_name_open = 1; np.ui.room_name_draft[0] = 0; accepted = 1;
    }
  } else if (np.ui.room_name_open) {
    char* draft = np.ui.room_name_draft; size_t length = strlen(draft);
    accepted = 1;
    if (action == NETPLAY_NAME_APPEND && text) {
      for (const unsigned char* next = (const unsigned char*)text; *next && length < sizeof np.ui.room_name_draft - 1; ++next)
        if (*next >= 32 && *next <= 126) draft[length++] = (char)*next;
      draft[length] = 0;
    } else if (action == NETPLAY_NAME_BACKSPACE) { if (length) draft[length - 1] = 0; }
    else if (action == NETPLAY_NAME_CLEAR) draft[0] = 0;
    else if (action == NETPLAY_NAME_CANCEL) np.ui.room_name_open = 0;
    else if (action == NETPLAY_NAME_CONFIRM) {
      if (np.ui.cmd != NETPLAY_CMD_NONE || np.ui.room_id || np.active || np.armed) accepted = 0;
      else {
        while (length && draft[length - 1] == ' ') draft[--length] = 0;
        const char* first = draft; while (*first == ' ') ++first;
        snprintf(np.ui.cmd_text, sizeof np.ui.cmd_text, "%s", *first ? first : "YAMPP room");
        np.ui.cmd_arg = 0; np.ui.cmd_rules = np.ui.rules;
        np.ui.cmd = NETPLAY_CMD_CREATE; np.ui.room_name_open = 0;
      }
    }
  }
  unlock();
  return accepted;
}

/* ---- minimal JSON ----------------------------------------------------------
 * Only what our own server emits: objects with string/int/bool/object/array
 * values, no escapes beyond \" and \\. */
static const char* skip_ws(const char* p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }
static const char* skip_value(const char* p) {
  p = skip_ws(p);
  if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; } return *p ? p + 1 : p; }
  if (*p == '{' || *p == '[') { char open = *p, close = open == '{' ? '}' : ']'; int depth = 0; do { if (*p == '"') { p = skip_value(p); continue; } if (*p == open) depth++; else if (*p == close) depth--; p++; } while (*p && depth > 0); return p; }
  while (*p && *p != ',' && *p != '}' && *p != ']') p++;
  return p;
}
/* Value of `key` inside the object at `obj`, or NULL. */
static const char* json_find(const char* obj, const char* key) {
  if (!obj) return NULL;
  const char* p = skip_ws(obj);
  if (*p != '{') return NULL;
  p++;
  size_t klen = strlen(key);
  for (;;) {
    p = skip_ws(p);
    if (*p != '"') return NULL;
    const char* k = p + 1; const char* kend = k; while (*kend && *kend != '"') kend++;
    p = skip_ws(kend + 1);
    if (*p != ':') return NULL;
    p = skip_ws(p + 1);
    if ((size_t)(kend - k) == klen && !strncmp(k, key, klen)) return p;
    p = skip_ws(skip_value(p));
    if (*p == ',') p++; else return NULL;
  }
}
static long json_int(const char* obj, const char* key, long fallback) {
  const char* v = json_find(obj, key);
  if (!v) return fallback;
  if (!strncmp(v, "true", 4)) return 1;
  if (!strncmp(v, "false", 5)) return 0;
  return strtol(v, NULL, 10);
}
static void json_str(const char* obj, const char* key, char* out, unsigned cap) {
  const char* v = json_find(obj, key); unsigned n = 0;
  out[0] = 0;
  if (!v || *v != '"') return;
  for (v++; *v && *v != '"' && n + 1 < cap; v++) { if (*v == '\\' && v[1]) v++; out[n++] = *v; }
  out[n] = 0;
}
/* Iterate objects of an array: *cursor starts at the '[' and advances. */
static const char* json_next(const char** cursor) {
  const char* p = skip_ws(*cursor);
  if (*p == '[') p++;
  p = skip_ws(p);
  if (*p == ',') p = skip_ws(p + 1);
  if (*p != '{') return NULL;
  *cursor = skip_value(p);
  return p;
}
static void json_escape(const char* in, char* out, unsigned cap) {
  unsigned n = 0;
  for (; *in && n + 3 < cap; in++) { if (*in == '"' || *in == '\\') out[n++] = '\\'; if ((unsigned char)*in < 0x20) continue; out[n++] = *in; }
  out[n] = 0;
}

/* ---- rules ----------------------------------------------------------------- */
static void rules_default(NetplayRules* r) { r->mode = 1; r->stock = 4; r->minutes = 8; r->items = 0; r->delay = 0; r->pause = 1; r->damage = 100; r->friendly_fire = 0; }
static void rules_clamp(NetplayRules* r) {
  if (r->mode < 0 || r->mode > 1) r->mode = 1;
  if (r->stock < 1) r->stock = 1; if (r->stock > 99) r->stock = 99;
  if (r->minutes < 0) r->minutes = 0; if (r->minutes > 99) r->minutes = 99;
  if (r->items < 0) r->items = 0; if (r->items > 5) r->items = 5;
  /* 0 keeps the delay automatic: it is resolved from the measured link when
   * the match starts, rather than guessed before anyone has connected. */
  if (r->delay < 0) r->delay = 0; if (r->delay > 10) r->delay = 10;
  r->pause = r->pause ? 1 : 0; r->friendly_fire = r->friendly_fire ? 1 : 0;
  if (r->damage < 50) r->damage = 50; if (r->damage > 200) r->damage = 200;
}
static void rules_from_json(const char* obj, NetplayRules* r) {
  rules_default(r);
  if (!obj) return;
  r->mode = (int)json_int(obj, "mode", r->mode); r->stock = (int)json_int(obj, "stock", r->stock); r->minutes = (int)json_int(obj, "minutes", r->minutes);
  r->items = (int)json_int(obj, "items", r->items); r->delay = (int)json_int(obj, "delay", r->delay); r->pause = (int)json_int(obj, "pause", r->pause);
  r->damage = (int)json_int(obj, "damage", r->damage); r->friendly_fire = (int)json_int(obj, "friendly_fire", r->friendly_fire);
  rules_clamp(r);
}
static int rules_to_json(const NetplayRules* r, char* out, unsigned cap) {
  return snprintf(out, cap, "{\"mode\":%d,\"stock\":%d,\"minutes\":%d,\"items\":%d,\"delay\":%d,\"pause\":%d,\"damage\":%d,\"friendly_fire\":%d}",
                  r->mode, r->stock, r->minutes, r->items, r->delay, r->pause, r->damage, r->friendly_fire);
}

/* ---- TLS -------------------------------------------------------------------- */
static int np_send(SOCKET s, const char* buf, int len) {
  if (np.tls_active && np.ssl) {
    ERR_clear_error();
    int r = SSL_write(np.ssl, buf, len);
    if (r > 0) return r;
    int err = SSL_get_error(np.ssl, r);
    if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
#ifdef _WIN32
      WSASetLastError(WSAEWOULDBLOCK);
#else
      errno = EWOULDBLOCK;
#endif
      return -1;
    }
    ERR_clear_error();
#ifdef _WIN32
    WSASetLastError(WSAECONNRESET);
#else
    errno = ECONNRESET;
#endif
    return -1;
  }
  return send(s, buf, len, NP_SEND_FLAGS);
}
static int np_recv(SOCKET s, char* buf, int len) {
  if (np.tls_active && np.ssl) {
    EnterCriticalSection(&np.send_lock);
    ERR_clear_error();
    int r = SSL_read(np.ssl, buf, len);
    if (r > 0) { LeaveCriticalSection(&np.send_lock); return r; }
    int err = SSL_get_error(np.ssl, r);
    LeaveCriticalSection(&np.send_lock);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
#ifdef _WIN32
      WSASetLastError(WSAEWOULDBLOCK);
#else
      errno = EWOULDBLOCK;
#endif
      return -1;
    }
    ERR_clear_error();
#ifdef _WIN32
    WSASetLastError(WSAECONNRESET);
#else
    errno = ECONNRESET;
#endif
    return r == 0 ? 0 : -1;
  }
  return recv(s, buf, len, 0);
}
static void np_tls_close(void) {
  if (np.ssl) { SSL_shutdown(np.ssl); SSL_free(np.ssl); np.ssl = NULL; }
  if (np.ssl_ctx) { SSL_CTX_free(np.ssl_ctx); np.ssl_ctx = NULL; }
  np.tls_active = 0;
}
static int np_tls_handshake(void) {
  np.ssl_ctx = SSL_CTX_new(TLS_client_method());
  if (!np.ssl_ctx) return 0;
  SSL_CTX_set_default_verify_paths(np.ssl_ctx);
#ifdef _WIN32
  /* Load Windows certificate store into OpenSSL for proper verification. */
  { X509_STORE* store = SSL_CTX_get_cert_store(np.ssl_ctx);
    HCERTSTORE sys = CertOpenSystemStoreW(0, L"ROOT");
    if (sys) {
      PCCERT_CONTEXT cert = NULL;
      while ((cert = CertEnumCertificatesInStore(sys, cert)) != NULL) {
        const unsigned char* cursor = cert->pbCertEncoded;
        X509* x509 = d2i_X509(NULL, &cursor, cert->cbCertEncoded);
        if (x509) { X509_STORE_add_cert(store, x509); X509_free(x509); }
      }
      CertCloseStore(sys, 0);
    }
  }
#endif
  SSL_CTX_set_verify(np.ssl_ctx, SSL_VERIFY_PEER, NULL);
  SSL_CTX_set_min_proto_version(np.ssl_ctx, TLS1_2_VERSION);
  np.ssl = SSL_new(np.ssl_ctx);
  if (!np.ssl) { np_tls_close(); return 0; }
  SSL_set_fd(np.ssl, (int)np.tcp);
  SSL_set_tlsext_host_name(np.ssl, np.http_host);
  SSL_set1_host(np.ssl, np.http_host);
  DWORD deadline = GetTickCount() + 8000;
  for (;;) {
    ERR_clear_error();
    int r = SSL_connect(np.ssl);
    if (r == 1) { np.tls_active = 1; return 1; }
    int err = SSL_get_error(np.ssl, r);
    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) break;
    if (np.quit || GetTickCount() > deadline) break;
    fd_set rfd, wfd; FD_ZERO(&rfd); FD_ZERO(&wfd);
    if (err == SSL_ERROR_WANT_READ) FD_SET(np.tcp, &rfd);
    else FD_SET(np.tcp, &wfd);
    struct timeval tv = {0, 50000};
    select((int)np.tcp + 1, &rfd, &wfd, NULL, &tv);
  }
  np_tls_close();
  return 0;
}
/* ---- sockets ---------------------------------------------------------------- */
/* Guest input submission only enqueues. The network thread owns SSL writes,
 * including retrying the same buffer after WANT_READ/WANT_WRITE. Congestion
 * must never sleep in the simulation thread or partially interleave JSON. */
static void tcp_send_line(const char* line) {
  if (!line) return;
  size_t length = strlen(line);
  if (length > 65530) return;
  EnterCriticalSection(&np.send_lock);
  if (np.tcp == INVALID_SOCKET || !np.connected || np.send_failed) {
    LeaveCriticalSection(&np.send_lock); return;
  }
  if (np.send_bytes + length + 1 > NP_SEND_LIMIT) {
    np.send_failed = 1; LeaveCriticalSection(&np.send_lock); return;
  }
  NetSend* item = malloc(sizeof *item + length + 1);
  if (!item) { np.send_failed = 1; LeaveCriticalSection(&np.send_lock); return; }
  item->next = NULL; item->length = length + 1; item->offset = 0;
  memcpy(item->data, line, length); item->data[length] = '\n';
  if (np.send_tail) np.send_tail->next = item; else np.send_head = item;
  np.send_tail = item; np.send_bytes += item->length;
  LeaveCriticalSection(&np.send_lock);
}
static int tcp_flush(void) {
  /* A bounded batch leaves time to receive input and process lobby commands. */
  for (unsigned batch = 0; batch < 32; ++batch) {
    EnterCriticalSection(&np.send_lock);
    NetSend* item = np.send_head;
    int failed = np.send_failed;
    LeaveCriticalSection(&np.send_lock);
    if (failed) return 0;
    if (!item) return 1;
    int n = np_send(np.tcp, item->data + item->offset, (int)(item->length - item->offset));
    if (n <= 0) return n < 0 && WSAGetLastError() == WSAEWOULDBLOCK;
    EnterCriticalSection(&np.send_lock);
    item->offset += (size_t)n;
    if (item->offset == item->length) {
      np.send_head = item->next;
      if (!np.send_head) np.send_tail = NULL;
      np.send_bytes -= item->length; free(item);
    }
    LeaveCriticalSection(&np.send_lock);
  }
  return 1;
}
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static unsigned b64_encode(const unsigned char* in, unsigned len, char* out, unsigned cap) {
  unsigned n = 0;
  for (unsigned i = 0; i < len; i += 3) {
    unsigned block = (unsigned)in[i] << 16;
    if (i + 1 < len) block |= (unsigned)in[i + 1] << 8;
    if (i + 2 < len) block |= in[i + 2];
    if (n + 4 >= cap) break;
    out[n++] = B64[(block >> 18) & 63];
    out[n++] = B64[(block >> 12) & 63];
    out[n++] = i + 1 < len ? B64[(block >> 6) & 63] : '=';
    out[n++] = i + 2 < len ? B64[block & 63] : '=';
  }
  out[n] = 0;
  return n;
}
static unsigned b64_decode(const char* in, unsigned char* out, unsigned cap) {
  unsigned block = 0, bits = 0, n = 0;
  for (; *in && *in != '"'; in++) {
    const char* at = strchr(B64, *in);
    if (!at || !*in) continue;
    block = (block << 6) | (unsigned)(at - B64);
    if ((bits += 6) < 8) continue;
    bits -= 8;
    if (n < cap) out[n++] = (unsigned char)((block >> bits) & 0xFF);
  }
  return n;
}
/* ---- match traffic ----------------------------------------------------------
 * Two paths carry the same packets. The lobby connection always works, because
 * it is the port the server already answers on, but it is a stream: one lost
 * segment holds every input behind it until the retransmission arrives, which
 * is felt as a freeze and then a burst of rollbacks. The datagram path has no
 * such ordering, and input packets do not need any -- each one names the exact
 * frames it carries and repeats the previous few, so a lost datagram is
 * repaired by the next one about sixteen milliseconds later.
 *
 * The datagram path is used only once packets have been seen arriving on it,
 * and is abandoned the moment it goes quiet, so a blocked UDP port or a
 * hostile middlebox costs nothing beyond the probes. */
static void udp_send(const void* payload, unsigned len) {
  /* The guest thread sends here while the network thread may be closing the
   * socket; send_lock is the same one the stream queue uses, and neither
   * caller holds it across anything that blocks. */
  EnterCriticalSection(&np.send_lock);
  if (np.udp != INVALID_SOCKET && np.udp_token[0] && len + NP_UDP_TOKEN <= 1500) {
    unsigned char datagram[1500];
    memcpy(datagram, np.udp_token, NP_UDP_TOKEN);
    memcpy(datagram + NP_UDP_TOKEN, payload, len);
    sendto(np.udp, (const char*)datagram, (int)(len + NP_UDP_TOKEN), 0,
           (struct sockaddr*)&np.udp_addr, sizeof np.udp_addr);
  }
  LeaveCriticalSection(&np.send_lock);
}
/* "1.2.3.4:5678" as the server wrote it. Numeric only: a peer address is
 * never a name to look up, and refusing to resolve one keeps a hostile entry
 * from turning into a DNS query. */
static int parse_endpoint(const char* text, struct sockaddr_in* out) {
  unsigned a, b, c, d, port;
  char tail;
  if (!text || !*text) return 0;
  if (sscanf(text, "%u.%u.%u.%u:%u%c", &a, &b, &c, &d, &port, &tail) != 5) return 0;
  if (a > 255 || b > 255 || c > 255 || d > 255 || port == 0 || port > 65535) return 0;
  memset(out, 0, sizeof *out);
  out->sin_family = AF_INET;
  out->sin_port = htons((unsigned short)port);
  { unsigned char* bytes = (unsigned char*)&out->sin_addr;
    bytes[0] = (unsigned char)a; bytes[1] = (unsigned char)b;
    bytes[2] = (unsigned char)c; bytes[3] = (unsigned char)d; }
  return 1;
}
/* Which guest port belongs to a lobby client id, or -1. */
static int port_of_id(uint16_t id) {
  for (int p = 0; p < 4; ++p) if (np.port_ids[p] == (int)id) return p;
  return -1;
}
/* One already-packed relay frame, straight to a player. The token in front is
 * theirs, which is how their client knows the datagram came from someone the
 * server put in this session with them. */
static void direct_send_to(const struct sockaddr_in* where, const char* token,
                           const void* packed, unsigned len) {
  EnterCriticalSection(&np.send_lock);
  if (np.udp != INVALID_SOCKET && token[0] && len + NP_UDP_TOKEN <= 1500) {
    unsigned char datagram[1500];
    memcpy(datagram, token, NP_UDP_TOKEN);
    memcpy(datagram + NP_UDP_TOKEN, packed, len);
    sendto(np.udp, (const char*)datagram, (int)(len + NP_UDP_TOKEN), 0,
           (const struct sockaddr*)where, sizeof *where);
  }
  LeaveCriticalSection(&np.send_lock);
}
/* Send to every addressed peer, preferring the direct path where one exists.
 *
 * The relay copy is only made when some addressed peer still needs it, so a
 * match where both paths opened stops touching the server entirely, and a
 * match where only one did keeps paying for only that one. */
static void relay_send(const void* payload, unsigned len, uint16_t to) {
  if (!np.connected || !np.session) return;
  unsigned char buffer[1400];
  if (len + sizeof(RelayHeader) > sizeof buffer) return;
  RelayHeader h = { NP_MAGIC, np.session, (uint16_t)np.local_id, to };
  memcpy(buffer, &h, sizeof h); memcpy(buffer + sizeof h, payload, len);
  unsigned packed = len + (unsigned)sizeof h;
  { int addressed = 0, through_relay = 0;
    for (int p = 0; p < 4; ++p) {
      if (!np.port_ids[p] || p == np.local_port) continue;
      if (to && (uint16_t)np.port_ids[p] != to) continue;
      addressed = 1;
      if (np_peer[p].direct_ok) direct_send_to(&np_peer[p].direct_addr, np_peer[p].token, buffer, packed);
      else through_relay = 1;
    }
    /* Before the player list exists there is nobody to address directly, and
     * the packet still has to reach the server. */
    if (addressed && !through_relay) return;
  }
  if (np.udp != INVALID_SOCKET) {
    udp_send(buffer, packed);
    /* Once the datagram path has proven itself the stream copy is dropped:
     * sending both doubles the traffic and puts the slower copy in front of
     * the lobby messages that still need the stream. */
    if (np.udp_ready) return;
  }
  if (np.tcp == INVALID_SOCKET) return;
  char encoded[1900], line[2048];
  b64_encode(buffer, packed, encoded, sizeof encoded);
  snprintf(line, sizeof line, "{\"op\":\"r\",\"d\":\"%s\"}", encoded);
  tcp_send_line(line);
}
static void udp_close(void) {
  EnterCriticalSection(&np.send_lock);
  if (np.udp != INVALID_SOCKET) { closesocket(np.udp); np.udp = INVALID_SOCKET; }
  np.udp_ready = np.udp_offered = 0; np.udp_token[0] = 0;
  LeaveCriticalSection(&np.send_lock);
  /* The socket the punched mappings belonged to is gone, so the paths are
   * too; they are re-proven on the new one. */
  for (int p = 0; p < 4; ++p) { np_peer[p].direct_ok = 0; np_peer[p].candidates = 0; }
  np.direct_peers = 0;
  lock(); np.ui.udp_active = 0; np.ui.direct_peers = 0; unlock();
}
/* Open the side channel the server advertised. Failure is not an error: the
 * stream path stays in place and nothing about the match changes. */
static void udp_open(const char* token, int port) {
  udp_close();
  if (!token || !*token || port <= 0 || port > 65535) return;
  if (strlen(token) != NP_UDP_TOKEN) return;
  np.udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (np.udp == INVALID_SOCKET) return;
#ifdef _WIN32
  { u_long nonblocking = 1; ioctlsocket(np.udp, FIONBIO, &nonblocking); }
  /* A datagram refused by an unreachable host must not fail later receives. */
  { DWORD off = 0; DWORD returned = 0;
    WSAIoctl(np.udp, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof off, NULL, 0, &returned, NULL, NULL); }
#else
  { int flags = fcntl(np.udp, F_GETFL, 0); if (flags >= 0) fcntl(np.udp, F_SETFL, flags | O_NONBLOCK); }
#endif
  EnterCriticalSection(&np.send_lock);
  np.udp_addr = np.server_addr; np.udp_addr.sin_port = htons((unsigned short)port);
  memcpy(np.udp_token, token, NP_UDP_TOKEN); np.udp_token[NP_UDP_TOKEN] = 0;
  LeaveCriticalSection(&np.send_lock);
  np.udp_offered = 1; np.udp_last_rx = np.udp_last_probe = GetTickCount();
  /* Bind now rather than on the first send, so the port is known and can be
   * offered to the other player while the session is still being set up. */
  { struct sockaddr_in any; memset(&any, 0, sizeof any);
    any.sin_family = AF_INET; any.sin_addr.s_addr = htonl(INADDR_ANY); any.sin_port = 0;
    bind(np.udp, (struct sockaddr*)&any, sizeof any); }
  np.local_udp[0] = 0;
  { /* The address this machine uses to reach the server is the one its own
     * network knows it by. A connected datagram socket reports it without
     * sending anything, and enumerating every interface would only produce
     * candidates that cannot route. */
    struct sockaddr_in mine; socklen_t mine_len = sizeof mine;
    SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe != INVALID_SOCKET) {
      if (!connect(probe, (struct sockaddr*)&np.udp_addr, sizeof np.udp_addr)
          && !getsockname(probe, (struct sockaddr*)&mine, &mine_len)) {
        struct sockaddr_in bound; socklen_t bound_len = sizeof bound;
        if (!getsockname(np.udp, (struct sockaddr*)&bound, &bound_len) && bound.sin_port) {
          unsigned char* a = (unsigned char*)&mine.sin_addr;
          snprintf(np.local_udp, sizeof np.local_udp, "%u.%u.%u.%u:%u",
                   a[0], a[1], a[2], a[3], (unsigned)ntohs(bound.sin_port));
        }
      }
      closesocket(probe);
    }
  }
  fprintf(stderr, "[netplay] datagram channel offered on port %d\n", port);
}
static void close_sockets(void) {
  udp_close();
  EnterCriticalSection(&np.send_lock);
  while (np.send_head) { NetSend* next = np.send_head->next; free(np.send_head); np.send_head = next; }
  np.send_tail = NULL; np.send_bytes = 0; np.send_failed = 0;
  np_tls_close();
  if (np.tcp != INVALID_SOCKET) { closesocket(np.tcp); np.tcp = INVALID_SOCKET; }
  np.connected = np.connecting = np.upgrading = 0; np.rx_len = 0;
  LeaveCriticalSection(&np.send_lock);
}

/* ---- session ---------------------------------------------------------------- */
static void end_session(const char* why) {
  if (!np.active && !np.armed) return;
  InterlockedExchange(&np.active, 0); InterlockedExchange(&np.armed, 0); InterlockedExchange(&np.start_pending, 0); InterlockedExchange(&np.guest_started, 0);
  unsigned char bye[2] = { PKT_BYE, (uint8_t)np.local_port };
  relay_send(bye, sizeof bye, 0);
  np.session = 0;
  lock(); np.ui.session_active = 0; np.ui.phase = np.connected ? (np.ui.room_id ? NETPLAY_PHASE_ROOM : NETPLAY_PHASE_LOBBY) : NETPLAY_PHASE_OFFLINE; unlock();
  memset(np_peer, 0, sizeof np_peer);
  np.peer_token[0] = 0; np.direct_peers = 0;
  lock(); np.ui.direct_peers = 0; unlock();
  InterlockedExchange(&np.interrupted, 0); np.interrupt_why[0] = 0; np.skip_frames = 0;
  np.resuming = 0;
  lock(); np.ui.interrupted = 0; np.ui.interrupt_ms = 0; np.ui.interrupt_text[0] = 0;
  np.ui.reconnecting = 0; np.ui.udp_active = 0; np.ui.frame_advantage = 0; unlock();
  char text[200]; snprintf(text, sizeof text, "Session ended: %s", why); status(text);
}

/* ---- interruptions ----------------------------------------------------------
 * A match that loses its opponent for a moment is held, not ended. Everything
 * that used to reach for end_session on the first sign of trouble comes here
 * instead: the simulation state, the rollback window and the lobby session all
 * stay exactly as they were, the players are told what is happening and for
 * how much longer, and play resumes from the same frame the instant input
 * arrives again. Only a link that stays dead past the grace period ends the
 * session, and then it says so for a reason the players can act on. */
static void np_interrupt(const char* why) {
  DWORD now = GetTickCount();
  int first;
  if (!np.active) return;
  lock();
  first = !InterlockedExchange(&np.interrupted, 1);
  if (first) np.interrupt_since = now;
  snprintf(np.interrupt_why, sizeof np.interrupt_why, "%s", why ? why : "Waiting for opponent");
  np.ui.interrupted = 1;
  np.ui.interrupt_ms = (int)(now - np.interrupt_since);
  np.ui.interrupt_limit_ms = NP_STALL_GRACE_MS;
  snprintf(np.ui.interrupt_text, sizeof np.ui.interrupt_text, "%s", np.interrupt_why);
  unlock();
  if (first) fprintf(stderr, "[netplay] match held: %s\n", why ? why : "waiting");
}
static void np_resume(void) {
  DWORD held;
  lock();
  if (!InterlockedExchange(&np.interrupted, 0)) { unlock(); return; }
  held = GetTickCount() - np.interrupt_since;
  np.interrupt_why[0] = 0;
  np.ui.interrupted = 0; np.ui.interrupt_ms = 0; np.ui.interrupt_text[0] = 0;
  unlock();
  fprintf(stderr, "[netplay] match resumed after %u ms\n", (unsigned)held);
}
/* Milliseconds the match has been held, or 0 when it is running normally. */
static DWORD np_interrupt_age(void) {
  return np.interrupted ? GetTickCount() - np.interrupt_since : 0;
}
static void store_input(int port, int epoch, uint32_t frame, const NetInput* in) {
  InputSlot* slot = &np.inputs[port][epoch & 1][frame % NP_RING];
  if (slot->valid && slot->epoch == epoch && slot->frame == frame) {
    if (memcmp(&slot->in, in, sizeof *in)) { InterlockedExchange(&np.desynced, 1); np.ui.desynced = 1; }
    return;
  }
  slot->valid = 1; slot->epoch = epoch; slot->frame = frame; slot->in = *in;
  if (rollback_running && epoch == np.epoch) {
    RbInput input; memcpy(&input, in, sizeof input);
    if (rb_receive(&rollback, (unsigned)port, frame, &input) < 0)
      InterlockedExchange(&np.desynced, 1);
  }
}
static const NetInput* get_input(int port, int epoch, uint32_t frame) {
  const InputSlot* slot = &np.inputs[port][epoch & 1][frame % NP_RING];
  return (slot->valid && slot->epoch == epoch && slot->frame == frame) ? &slot->in : NULL;
}
/* How far behind the peers we believe we are, as one number to put on the
 * wire. With more than two players the furthest-ahead peer is the one that
 * matters: correcting towards anyone slower would leave that peer predicting. */
static int np_local_advantage(void) {
  int advantage = 0, have = 0;
  for (int p = 0; p < 4; ++p) {
    if (!np.port_ids[p] || p == np.local_port) continue;
    int value = timesync_advantage(&np_peer[p].sync, np.frame, np_peer[p].last_frame);
    if (!have || value < advantage) { advantage = value; have = 1; }
  }
  return have ? advantage : 0;
}
static void send_inputs(uint32_t upto, unsigned max_count) {
  unsigned char buffer[1200]; InputHeader h; unsigned count = 0;
  uint32_t first = upto + 1 > max_count ? upto + 1 - max_count : 0;
  NetInput list[64];
  lock();
  while (!get_input(np.local_port, np.epoch, upto)) {
    if (!upto || upto <= first) { unlock(); return; }
    --upto;
  }
  for (uint32_t f = first; f <= upto && count < 64; f++) { const NetInput* in = get_input(np.local_port, np.epoch, f); if (!in) { count = 0; first = f + 1; continue; } list[count++] = *in; }
  h.type = PKT_INPUT; h.epoch = (uint8_t)np.epoch; h.port = (uint8_t)np.local_port; h.count = (uint8_t)count;
  h.last_frame = upto; h.ack = np.frame;
  { int advantage = np_local_advantage();
    h.advantage = (int8_t)(advantage > 127 ? 127 : advantage < -128 ? -128 : advantage); }
  /* A peer that has not run a frame yet has no hash to send. Saying so with a
     frame number that can never be compared keeps the far side from reading the
     empty value as a real hash of frame 0 and calling it a desync. */
  uint32_t hash_end = rollback_running ? rollback_hash_sent : np.frame;
  h.hash_frame = hash_end ? hash_end - 1 : 0xFFFFFFFFu;
  h.hash = hash_end ? np.hashes[(hash_end - 1) & 255] : 0;
  unlock();
  memcpy(buffer, &h, sizeof h); memcpy(buffer + sizeof h, list, count * sizeof(NetInput));
  relay_send(buffer, (unsigned)(sizeof h + count * sizeof(NetInput)), 0);
}
/* Round-trip measurement. The lobby ping measures the path to the server,
 * which is not the path the inputs take and is not what the clock correction
 * or the automatic delay need; both of those need the time to the other
 * player. One outstanding probe at a time keeps the samples honest. */
static void ping_peers(void) {
  DWORD now = GetTickCount();
  for (int p = 0; p < 4; ++p) {
    if (!np.port_ids[p] || p == np.local_port) continue;
    NetPeer* peer = &np_peer[p];
    if (peer->ping_outstanding && now - peer->ping_sent < NP_PING_INTERVAL_MS * 5) continue;
    if (now - peer->ping_sent < NP_PING_INTERVAL_MS) continue;
    peer->ping_sent = now; peer->ping_tick = (uint32_t)now; peer->ping_outstanding = 1;
    PingPacket ping = { PKT_PING, (uint8_t)np.local_port, peer->ping_tick };
    relay_send(&ping, sizeof ping, (uint16_t)np.port_ids[p]);
  }
}
/* The peer round-trip the match is actually running on, in milliseconds. */
static int np_peer_ping(void) {
  unsigned worst = 0; int have = 0;
  for (int p = 0; p < 4; ++p) {
    if (!np.port_ids[p] || p == np.local_port) continue;
    unsigned rtt = timesync_rtt(&np_peer[p].sync);
    if (!rtt) continue;
    if (rtt > worst) worst = rtt;
    have = 1;
  }
  return have ? (int)worst : -1;
}
static void handle_relay(const unsigned char* data, int len) {
  if (len < (int)sizeof(RelayHeader)) return;
  RelayHeader h; memcpy(&h, data, sizeof h);
  if (h.magic != NP_MAGIC || h.session != np.session || !np.session) return;
  const unsigned char* payload = data + sizeof h; len -= (int)sizeof h;
  if (len < 1) return;
  int from_port = -1;
  for (int p = 0; p < 4; p++) if (np.port_ids[p] == h.from) from_port = p;
  if (from_port < 0 || from_port == np.local_port) return;
  DWORD arrived = GetTickCount();
  np.last_rx = arrived;
  np.peer_seen[from_port] = 1;
  np_peer[from_port].last_rx = arrived;
  /* Anything at all from a peer that had said goodbye means they are back. */
  if (np_peer[from_port].gone && payload[0] != PKT_BYE) {
    np_peer[from_port].gone = 0;
    fprintf(stderr, "[netplay] port %d came back\n", from_port + 1);
  }
  if (payload[0] == PKT_HELLO) return;
  if (payload[0] == PKT_PING && len >= (int)sizeof(PingPacket)) {
    PingPacket ping; memcpy(&ping, payload, sizeof ping);
    ping.type = PKT_PONG; ping.port = (uint8_t)np.local_port;
    relay_send(&ping, sizeof ping, h.from);
    return;
  }
  if (payload[0] == PKT_PONG && len >= (int)sizeof(PingPacket)) {
    PingPacket pong; memcpy(&pong, payload, sizeof pong);
    NetPeer* peer = &np_peer[from_port];
    if (peer->ping_outstanding && pong.tick == peer->ping_tick) {
      peer->ping_outstanding = 0;
      lock(); timesync_rtt_sample(&peer->sync, (unsigned)(arrived - peer->ping_sent)); unlock();
    }
    return;
  }
  if (payload[0] == PKT_BYE) {
    /* Leaving and dropping out look identical on the wire, and a client that
     * is reconnecting sends nothing at all while it does so. Hold the match
     * and let the grace period decide which of the two this was. */
    np_peer[from_port].gone = 1; np_peer[from_port].gone_at = arrived;
    np_interrupt("Opponent left the match");
    return;
  }
  if (payload[0] == PKT_REQUEST && len >= 5) {
    uint32_t from = 0; memcpy(&from, payload + 1, 4);
    lock(); uint32_t latest = np.frame + (uint32_t)np.delay; unlock();
    if (latest >= from) send_inputs(latest, latest - from + 1 > 64 ? 64 : latest - from + 1);
    return;
  }
  if (payload[0] != PKT_INPUT || len < (int)sizeof(InputHeader)) return;
  InputHeader ih; memcpy(&ih, payload, sizeof ih);
  if (ih.port != from_port || !ih.count || ih.count > 64 || ih.last_frame == UINT32_MAX || ih.last_frame + 1 < ih.count ||
      len != (int)(sizeof ih + ih.count * sizeof(NetInput))) return;
  lock();
  int epoch_distance = (uint8_t)(ih.epoch - (uint8_t)np.epoch);
  if (epoch_distance > 1) { unlock(); return; }
  uint32_t receive_frame = epoch_distance ? 0 : np.frame;
  if ((ih.last_frame >= receive_frame && ih.last_frame - receive_frame >= NP_RING / 2) ||
      (ih.last_frame < receive_frame && receive_frame - ih.last_frame >= NP_RING / 2)) { unlock(); return; }
  for (unsigned i = 0; i < ih.count; i++) {
    uint32_t frame = ih.last_frame - ih.count + 1 + i; NetInput in; memcpy(&in, payload + sizeof ih + i * sizeof in, sizeof in);
    /* Expand the wire byte so ancient ring entries cannot alias at wrap. */
    store_input(from_port, np.epoch + epoch_distance, frame, &in);
  }
  np.remote_hash_epoch[from_port] = ih.epoch; np.remote_hash_frame[from_port] = ih.hash_frame; np.remote_hash[from_port] = ih.hash; np.remote_hash_new[from_port] = 1;
  /* The two halves of the clock correction: where they say they are, and how
   * far behind us they measure themselves to be. */
  if (!epoch_distance && ih.ack + 1u > np_peer[from_port].last_frame)
    np_peer[from_port].last_frame = ih.ack;
  np_peer[from_port].remote_advantage = ih.advantage;
  unlock();
}

#include "netplay_mods.inc"
#include "netplay_profiles.inc"

/* ---- lobby protocol ----------------------------------------------------------- */
static void ui_set_phase(int phase) { lock(); np.ui.phase = phase; unlock(); }
static void parse_room(const char* room) {
  lock();
  np.ui.room_id = (int)json_int(room, "id", 0);
  json_str(room, "name", np.ui.room_name, sizeof np.ui.room_name);
  np.ui.is_host = json_int(room, "host_id", -1) == np.local_id;
  rules_from_json(json_find(room, "rules"), &np.ui.rules);
  np.ui.player_count = 0; np.ui.all_ready = 1;
  const char* cursor = json_find(room, "players"); const char* p;
  while (cursor && (p = json_next(&cursor)) && np.ui.player_count < NETPLAY_MAX_PLAYERS) {
    NetplayPlayer* pl = &np.ui.players[np.ui.player_count++];
    pl->id = (int)json_int(p, "id", 0); pl->ready = (int)json_int(p, "ready", 0); pl->port = (int)json_int(p, "port", 0);
    json_str(p, "name", pl->name, sizeof pl->name);
    json_str(p, "avatar", pl->avatar, sizeof pl->avatar);
    if (!pl->ready) np.ui.all_ready = 0;
  }
  if (np.ui.player_count < 2) np.ui.all_ready = 0;
  np.ui.phase = np.ui.room_id ? NETPLAY_PHASE_ROOM : NETPLAY_PHASE_LOBBY;
  unlock();
}
static void disconnect(const char* why);
static void handle_line(const char* line) {
  char op[32]; json_str(line, "op", op, sizeof op);
  if (!strcmp(op, "r")) {
    const char* value = json_find(line, "d");
    if (value && *value == '"') {
      unsigned char packet[1500];
      unsigned n = b64_decode(value + 1, packet, sizeof packet);
      if (n) handle_relay(packet, (int)n);
    }
    return;
  }
  if (!strcmp(op, "welcome") || !strcmp(op, "start")) {
    char sync[32]; json_str(line, "sync", sync, sizeof sync);
    if (json_int(line, "version", 0) != 2 || strcmp(sync, "rollback-v2")) {
      disconnect("Incompatible netplay server; this build needs the rollback v2 update"); return;
    }
  }
  if (!strcmp(op, "avatar_part")) { profile_picture_part(line); return; }
  if (!strcmp(op, "welcome")) {
    const char* features=json_find(line,"features");
    profile_local.supported=features&&strstr(features,"\"profile-v1\"")!=NULL;
    lock();profile_local.dirty=1;unlock();
    memset(profile_incoming,0,sizeof profile_incoming);
    nm_welcome(line);
    if (!nm.has_compat || (g_mex_active && !nm.has_upstream)) { disconnect("The server needs the current netplay update."); return; }
    np.local_id = (int)json_int(line, "id", 0);
    { char token[NP_UDP_TOKEN + 8]; json_str(line, "udp_token", token, sizeof token);
      int udp_port = (int)json_int(line, "udp_port", 0);
      if (udp_port > 0 && token[0]) udp_open(token, udp_port); }
    if (np.resuming && np.session) {
      /* Reclaim the match we were already in rather than landing in the lobby. */
      char line_out[128];
      snprintf(line_out, sizeof line_out, "{\"op\":\"resume\",\"session\":%u}", (unsigned)np.session);
      lock(); np.ui.local_id = np.local_id; unlock();
      tcp_send_line(line_out);
      status("Rejoining the match...");
      return;
    }
    lock(); np.ui.local_id = np.local_id; np.ui.phase = NETPLAY_PHASE_LOBBY; unlock();
    status("Connected to server");
    tcp_send_line("{\"op\":\"list\"}");
  } else if (!strcmp(op, "pong")) {
    lock(); np.ui.ping_ms = (int)(GetTickCount() - np.last_ping); unlock();
    np.ping_reported = 0;
  } else if (!strcmp(op, "rooms")) {
    lock();
    np.ui.room_count = 0;
    const char* cursor = json_find(line, "rooms"); const char* r;
    while (cursor && (r = json_next(&cursor)) && np.ui.room_count < NETPLAY_MAX_ROOMS) {
      NetplayRoom* room = &np.ui.rooms[np.ui.room_count++];
      room->id = (int)json_int(r, "id", 0); room->players = (int)json_int(r, "players", 0); room->max = (int)json_int(r, "max", 2); room->state = (int)json_int(r, "state", 0);
      json_str(r, "name", room->name, sizeof room->name); json_str(r, "host", room->host, sizeof room->host);
      const char* room_rules = json_find(r, "rules");
      room->rules_known = room_rules && *room_rules == '{';
      rules_from_json(room_rules, &room->rules);
    }
    unlock();
  } else if (!strcmp(op, "build_required")) {
    nm_upstream_required(line);
  } else if (!strcmp(op, "mods_required")) {
    nm_requirements(line);
  } else if (!strcmp(op, "room")) {
    const char* room = json_find(line, "room");
    if (room && *room == '{') { if (nm_room_ack(room)) { parse_room(room); profile_room_pictures(); } }
    else { nm_clear(1); lock(); np.ui.room_id = 0; np.ui.player_count = 0; np.ui.room_name_open = 0; np.ui.phase = NETPLAY_PHASE_LOBBY; unlock(); }
  } else if (!strcmp(op, "error")) {
    if (nm.awaiting_room || nm.pending_join) nm_clear(!np.ui.room_id);
    char message[160]; json_str(line, "message", message, sizeof message); char text[200]; snprintf(text, sizeof text, "Server: %s", message);
    char error_code[48]; json_str(line, "code", error_code, sizeof error_code);
    if (!strcmp(error_code, "compatibility_mismatch")) snprintf(text, sizeof text, "Match the host build, game data and aspect ratio.");
    status(text);
  } else if (!strcmp(op, "start")) {
    if (np.active || np.armed) { status("Ignored duplicate session start"); return; }
    if (!nm_start_ok(line)) { tcp_send_line("{\"op\":\"leave\"}"); status("Matching costumes were not ready. Session canceled."); return; }
    np.session = (uint32_t)json_int(line, "session", 0); np.seed = (uint32_t)json_int(line, "seed", 1);
    NetplayRules rules; rules_from_json(json_find(line, "rules"), &rules);
    /* One delay for the whole session, resolved by the relay from the round
     * trips both clients reported to it -- which is the path their inputs
     * actually take to each other. It must be identical on both sides: the
     * frames before the delay are seeded as known-empty input on every port,
     * so two clients disagreeing about that boundary would contradict each
     * other the first time someone held a direction on frame zero. */
    np.delay = (int)json_int(line, "delay", rules.delay ? rules.delay : 3);
    if (np.delay < 1) np.delay = 1;
    if (np.delay > 10) np.delay = 10;
    memset(np.port_ids, 0, sizeof np.port_ids); memset(np.peer_seen, 0, sizeof np.peer_seen); np.local_port = -1;
    const char* cursor = json_find(line, "players"); const char* p;
    while (cursor && (p = json_next(&cursor))) {
      int id = (int)json_int(p, "id", 0), port = (int)json_int(p, "port", 0);
      if (port < 0 || port >= 4 || id <= 0 || id > 65535 || np.port_ids[port]) { status("Invalid session player list"); np.session = 0; return; }
      for (int used = 0; used < 4; ++used) if (np.port_ids[used] == id) { status("Duplicate session player"); np.session = 0; return; }
      np.port_ids[port] = id; if (id == np.local_id) np.local_port = port;
    }
    if (np.local_port < 0 || !np.session) { status("Invalid session from server"); return; }
    lock(); np.ui.rules = rules; np.ui.delay = np.delay; np.ui.phase = NETPLAY_PHASE_STARTING; np.ui.session_active = 1; np.ui.desynced = 0; np.ui.stalled_ms = 0; unlock();
    memset(np.inputs, 0, sizeof np.inputs); np.epoch = 0; np.frame = 0; InterlockedExchange(&np.desynced, 0);
    memset(np_peer, 0, sizeof np_peer);
    for (int p = 0; p < 4; ++p) timesync_reset(&np_peer[p].sync);
    np.skip_frames = 0; np.resuming = 0;
    InterlockedExchange(&np.interrupted, 0); np.interrupt_why[0] = 0;
    lock(); np.ui.interrupted = 0; np.ui.interrupt_ms = 0; np.ui.interrupt_text[0] = 0;
    np.ui.reconnecting = 0; np.ui.clock_skips = 0; np.ui.rollbacks = 0; np.ui.rollback_frames = 0; unlock();
    InterlockedExchange(&np.guest_started, 0); InterlockedExchange(&np.armed, 1); np.last_hello = 0; np.last_rx = GetTickCount();
    status("Connecting to players...");
  } else if (!strcmp(op, "held")) {
    /* The other player's connection dropped and the server is keeping their
     * place. Say so rather than leaving the match apparently frozen. */
    if (np.active) {
      long seconds = json_int(line, "seconds", 0);
      char text[96];
      if (seconds > 0) snprintf(text, sizeof text, "Opponent is reconnecting (up to %lds)", seconds);
      else snprintf(text, sizeof text, "Opponent is reconnecting");
      np_interrupt(text);
    }
  } else if (!strcmp(op, "resumed")) {
    /* The server still had the session and has put us back in it, with a new
     * client id and a fresh datagram token. Our port assignment and the seed
     * are unchanged, so the simulation carries on from the frame it held on. */
    int ok = (int)json_int(line, "ok", 0);
    if (!ok) { np.resuming = 0; end_session("The match could not be rejoined"); return; }
    int previous = np.local_id;
    np.local_id = (int)json_int(line, "id", np.local_id);
    for (int p = 0; p < 4; ++p) if (np.port_ids[p] == previous) np.port_ids[p] = np.local_id;
    const char* players = json_find(line, "players"); const char* entry;
    while (players && (entry = json_next(&players))) {
      int id = (int)json_int(entry, "id", 0), port = (int)json_int(entry, "port", -1);
      if (port >= 0 && port < 4 && id > 0 && id <= 65535) np.port_ids[port] = id;
    }
    { char token[NP_UDP_TOKEN + 8]; json_str(line, "udp_token", token, sizeof token);
      int udp_port = (int)json_int(line, "udp_port", 0);
      if (udp_port > 0 && token[0]) udp_open(token, udp_port); }
    np.resuming = 0; np.last_rx = GetTickCount();
    lock(); np.ui.local_id = np.local_id; np.ui.reconnecting = 0; np.ui.phase = NETPLAY_PHASE_PLAYING; unlock();
    np_resume();
    status("Rejoined the match");
    fprintf(stderr, "[netplay] rejoined session %u as id %d\n", np.session, np.local_id);
  } else if (!strcmp(op, "peers")) {
    /* Where the other players can be reached, and the tokens that make a
     * datagram from them believable. The server is the only thing that knows
     * both, which is what keeps the addresses out of reach of anyone who is
     * not in this session. */
    if ((uint32_t)json_int(line, "session", 0) != np.session || !np.session) return;
    json_str(line, "token", np.peer_token, sizeof np.peer_token);
    const char* cursor = json_find(line, "peers"); const char* entry;
    while (cursor && (entry = json_next(&cursor))) {
      int id = (int)json_int(entry, "id", 0);
      int port = port_of_id((uint16_t)id);
      if (port < 0 || port == np.local_port) continue;
      NetPeer* peer = &np_peer[port];
      char token[NP_UDP_TOKEN + 8]; json_str(entry, "token", token, sizeof token);
      if (strlen(token) != NP_UDP_TOKEN) continue;
      memcpy(peer->token, token, NP_UDP_TOKEN); peer->token[NP_UDP_TOKEN] = 0;
      char address[64];
      int found = 0;
      json_str(entry, "addr", address, sizeof address);
      if (parse_endpoint(address, &peer->candidate[found])) ++found;
      json_str(entry, "local", address, sizeof address);
      if (found < NP_CANDIDATES && parse_endpoint(address, &peer->candidate[found])) ++found;
      peer->candidates = found;
      peer->direct_probe = 0;       /* probe on the next service, not in 250 ms */
      if (found) fprintf(stderr, "[netplay] port %d has %d address(es) to try directly\n", port, found);
    }
  } else if (!strcmp(op, "ended")) {
    end_session("Server ended the session");
  }
}

static int resolve_server(const char* text, struct sockaddr_in* out) {
  char host[160]; unsigned short port = 0;
  snprintf(host, sizeof host, "%s", text);
  /* Strip optional scheme. "http://" forces plaintext upgrade (custom/local
   * servers); everything else uses TLS when a path is present. */
  np.http_path[0] = 0; np.use_tls = 0;
  int explicit_plaintext = 0, has_scheme = 0;
  if (!strncmp(host, "http://", 7)) { memmove(host, host + 7, strlen(host + 7) + 1); explicit_plaintext = 1; has_scheme = 1; }
  else if (!strncmp(host, "https://", 8)) { memmove(host, host + 8, strlen(host + 8) + 1); has_scheme = 1; }
  char* slash = strchr(host, '/');
  if (slash) { snprintf(np.http_path, sizeof np.http_path, "%s", slash); *slash = 0; }
  else if (has_scheme) { snprintf(np.http_path, sizeof np.http_path, "/"); }
  char* colon = strrchr(host, ':');
  if (colon) { *colon = 0; port = (unsigned short)atoi(colon + 1); }
  if (np.http_path[0]) {
    np.use_tls = has_scheme ? !explicit_plaintext : is_official(text);
    if (!port) port = np.use_tls ? 443 : 80;
  } else {
    if (!port) port = NP_DEFAULT_PORT;
  }
  if (!host[0]) return 0;
  snprintf(np.http_host, sizeof np.http_host, "%s", host);
  struct addrinfo hints = {0}, *result = NULL; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, NULL, &hints, &result) || !result) return 0;
  *out = *(struct sockaddr_in*)result->ai_addr; out->sin_port = htons(port);
  freeaddrinfo(result);
  return 1;
}
/* A failed attempt during a reconnect must not look like going offline: the
 * match is still there, the aspect lock must stay held for it, and the loop
 * will try again shortly. */
static void connect_failed(const char* why) {
  status(why);
  if (np.resuming && np.active) { ui_set_phase(NETPLAY_PHASE_PLAYING); return; }
  nm_release_aspect();
  ui_set_phase(NETPLAY_PHASE_OFFLINE);
}
static void start_connect(void) {
  close_sockets();
  nm.hello_sent = nm.has_protocol = nm.has_compat = 0;
  if (!nm_bind_aspect()) { connect_failed("Update the renderer before using Online."); return; }
  if (!nm.compat_ready) {
    nm.connect_pending = 1;
    ui_set_phase(NETPLAY_PHASE_CONNECTING);
    nm_request_compatibility();
    status("Checking game files. Connection queued.");
    return;
  }
  nm.connect_pending = 0;
  lock(); np.ui.ping_ms = -1; unlock();
  char server[128], name[32];
  lock(); snprintf(server, sizeof server, "%s", np.ui.server); snprintf(name, sizeof name, "%s", np.ui.name); unlock();
  if (!resolve_server(server, &np.server_addr)) { connect_failed("Cannot resolve server address"); return; }
  np.tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (np.tcp == INVALID_SOCKET) { connect_failed("Cannot create socket"); return; }
#ifdef _WIN32
  { u_long nonblocking = 1; if (ioctlsocket(np.tcp, FIONBIO, &nonblocking) != 0) { closesocket(np.tcp); np.tcp = INVALID_SOCKET; connect_failed("Socket setup failed"); return; } }
#else
  { int flags = fcntl(np.tcp, F_GETFL, 0); if (flags < 0 || fcntl(np.tcp, F_SETFL, flags | O_NONBLOCK) < 0) { closesocket(np.tcp); np.tcp = INVALID_SOCKET; connect_failed("Socket setup failed"); return; } }
#endif
  { int nodelay = 1; setsockopt(np.tcp, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof nodelay); }
  connect(np.tcp, (struct sockaddr*)&np.server_addr, sizeof np.server_addr);
  np.connecting = 1; np.connect_started = GetTickCount();
  ui_set_phase(NETPLAY_PHASE_CONNECTING);
  status(nm.compat_ready ? "Connecting..." : "Checking game files...");
}
static void disconnect(const char* why);
static void link_lost(const char* why);
static void send_hello(void) {
  if (nm.hello_sent || !nm.compat_ready) return;
  if (!nm_aspect_valid()) { disconnect("Aspect setting changed. Reconnect to Online."); return; }
  char name[32], escaped[80];
  lock(); snprintf(name, sizeof name, "%s", np.ui.name); unlock();
  if (!name[0]) snprintf(name, sizeof name, "Player");
  json_escape(name, escaped, sizeof escaped);
  char line[900]; snprintf(line, sizeof line, "{\"op\":\"hello\",\"name\":\"%s\",\"version\":2,\"sync\":\"rollback-v2\",\"features\":[\"mods-v1\",\"compat-v1\",\"profile-v1\",\"upstream-builds-v1\"],\"compatibility\":{\"schema\":1,\"fingerprint\":\"%s\",\"runtime\":\"%s\",\"game\":\"%s\"}}", escaped, nm.fingerprint, nm.runtime_hash, nm.game_hash);
  if(g_mex_active){size_t n=strlen(line);snprintf(line+n-1,sizeof line-n+1,",\"upstream_build\":{\"id\":\"akaneia\",\"version\":\"%s\",\"sha256\":\"%s\"}}",AKANEIA_RELEASE,AKANEIA_SHA256);}
  nm.hello_sent = 1; tcp_send_line(line);
}
static void finish_connect(void) {
  /* Due immediately: the first measurement has to exist before the player
   * can start a room, not two seconds later. */
  np.connected = 1; np.connecting = 0; np.last_ping = GetTickCount() - 2000;
  np.ping_reported = 0;
  if (np.http_path[0]) {
    if (np.use_tls && !np_tls_handshake()) { link_lost("TLS handshake failed"); return; }
    /* Ask the web server to hand the connection over; the protocol starts
     * once it answers 101. */
    char request[512];
    int n = snprintf(request, sizeof request,
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: melee-netplay\r\nConnection: Upgrade\r\n"
                     "Cache-Control: no-cache\r\n\r\n", np.http_path, np.http_host);
    { DWORD send_deadline = GetTickCount() + 5000;
    for (int sent = 0; sent < n; ) {
      int w = np_send(np.tcp, request + sent, n - sent);
      if (w <= 0) { int e = WSAGetLastError(); if (e == WSAEWOULDBLOCK || e == EAGAIN) { if (np.quit || GetTickCount() > send_deadline) { link_lost("Send timed out"); return; } Sleep(1); continue; } link_lost("Could not reach the server"); return; }
      sent += w;
    } }
    np.upgrading = 1;
    status(nm.compat_ready ? "Connecting..." : "Checking game files...");
    return;
  }
  send_hello();
}
static void disconnect(const char* why) {
  end_session(why);
  nm_clear(1); nm_release_aspect(); nm.has_protocol = nm.has_compat = nm.hello_sent = 0;
  profile_local.supported=0;memset(profile_incoming,0,sizeof profile_incoming);
  close_sockets();
  lock(); np.ui.room_id = 0; np.ui.player_count = 0; np.ui.room_count = 0; np.ui.room_name_open = 0; np.ui.phase = NETPLAY_PHASE_OFFLINE; np.ui.ping_ms = -1; unlock();
  status(why);
}
/* The lobby connection went away on its own.
 *
 * With no match running this is an ordinary disconnect. With a match running
 * it is not: the session id, the simulation, the rollback window and the
 * other player are all still there, and the only thing that has failed is a
 * socket. Rebuild it underneath the players and rejoin the same session. The
 * aspect lock and the verified game files are deliberately kept, because
 * neither may change in the middle of a match. */
static void link_lost(const char* why) {
  if (!np.active) { disconnect(why); return; }
  if (!np.resuming) {
    np.resuming = 1; np.resume_since = GetTickCount();
    np.reconnect_at = GetTickCount() - NP_RECONNECT_BACKOFF_MS;
    fprintf(stderr, "[netplay] lobby link lost during a match (%s); reconnecting\n", why ? why : "unknown");
  }
  close_sockets();
  nm.hello_sent = nm.has_protocol = nm.has_compat = 0;
  lock(); np.ui.reconnecting = 1; unlock();
  np_interrupt("Reconnecting to the server");
}
static void process_command(void) {
  lock();
  int cmd = np.ui.cmd, arg = np.ui.cmd_arg; char text[48]; NetplayRules rules = np.ui.cmd_rules;
  snprintf(text, sizeof text, "%s", np.ui.cmd_text);
  if (cmd == NETPLAY_CMD_CONNECT) { snprintf(np.ui.server, sizeof np.ui.server, "%s", np.ui.edit_server); snprintf(np.ui.name, sizeof np.ui.name, "%s", np.ui.edit_name); }
  np.ui.cmd = NETPLAY_CMD_NONE;
  unlock();
  if (cmd == NETPLAY_CMD_NONE) return;
  if (nm_command(cmd, arg, text, &rules)) return;
  char line[512], escaped[120], rules_text[200];
  switch (cmd) {
  case NETPLAY_CMD_CONNECT: aurora_link_netplay_save_settings(is_official(np.ui.server) ? "" : np.ui.server, np.ui.name); start_connect(); break;
  case NETPLAY_CMD_DISCONNECT: disconnect("Disconnected"); break;
  case NETPLAY_CMD_REFRESH: tcp_send_line("{\"op\":\"list\"}"); break;
  case NETPLAY_CMD_CREATE:
    rules_clamp(&rules); json_escape(text[0] ? text : "Melee room", escaped, sizeof escaped); rules_to_json(&rules, rules_text, sizeof rules_text);
    snprintf(line, sizeof line, "{\"op\":\"create\",\"name\":\"%s\",\"max\":2,\"rules\":%s}", escaped, rules_text); tcp_send_line(line); break;
  case NETPLAY_CMD_JOIN: snprintf(line, sizeof line, "{\"op\":\"join\",\"room\":%d}", arg); tcp_send_line(line); break;
  case NETPLAY_CMD_LEAVE: nm.canceled = 1; tcp_send_line("{\"op\":\"leave\"}"); break;
  case NETPLAY_CMD_READY: snprintf(line, sizeof line, "{\"op\":\"ready\",\"ready\":%d}", arg ? 1 : 0); tcp_send_line(line); break;
  case NETPLAY_CMD_START: tcp_send_line("{\"op\":\"start\"}"); break;
  case NETPLAY_CMD_RULES: rules_clamp(&rules); rules_to_json(&rules, rules_text, sizeof rules_text); snprintf(line, sizeof line, "{\"op\":\"rules\",\"rules\":%s}", rules_text); tcp_send_line(line); break;
  case NETPLAY_CMD_CLOSE: InterlockedExchange(&np.lobby_open, 0); lock(); np.ui.open = 0; unlock(); break;
  case NETPLAY_CMD_END_SESSION: end_session("Left the session"); tcp_send_line("{\"op\":\"leave\"}"); break;
  }
}
/* Drain the datagram socket and keep the server's view of our address fresh.
 *
 * The server can only reach us at the address our own datagrams came from, so
 * probes continue for the life of the session: they keep a NAT mapping open
 * and re-register us if it changes. Traffic arriving here is the proof that
 * the path works in both directions, and its absence is the signal to fall
 * back to the stream before a single frame is lost to it. */
/* A datagram that arrived with our own token in front of it came from a
 * player the server put in this session with us. The address it came from is
 * where that player is actually reachable -- which is not necessarily any of
 * the addresses we were told to try, because a NAT rewrites the port -- so it
 * replaces the candidate we had, and their traffic goes there from now on. */
static void direct_received(int port, const struct sockaddr_in* from, DWORD now) {
  NetPeer* peer = &np_peer[port];
  int moved = peer->direct_addr.sin_addr.s_addr != from->sin_addr.s_addr
           || peer->direct_addr.sin_port != from->sin_port;
  peer->direct_addr = *from;
  peer->direct_rx = now;
  if (!peer->direct_ok || moved) {
    unsigned char* a = (unsigned char*)&from->sin_addr;
    if (!peer->direct_ok) {
      peer->direct_ok = 1;
      lock(); ++np.direct_peers; np.ui.direct_peers = np.direct_peers; unlock();
      fprintf(stderr, "[netplay] direct path to port %d open at %u.%u.%u.%u:%u;"
                      " their inputs no longer go through the server\n",
              port, a[0], a[1], a[2], a[3], (unsigned)ntohs(from->sin_port));
    } else {
      fprintf(stderr, "[netplay] direct path to port %d moved to %u.%u.%u.%u:%u\n",
              port, a[0], a[1], a[2], a[3], (unsigned)ntohs(from->sin_port));
    }
  }
}
/* Send the same registration packet to every address a peer might answer on.
 * Both players do this at once, which is what opens the two mappings: the
 * first packet out of each NAT is dropped by the other, and the second gets
 * through the hole the first one made. */
static void punch_peers(DWORD now) {
  if (!np.allow_direct) return;
  for (int p = 0; p < 4; ++p) {
    if (!np.port_ids[p] || p == np.local_port) continue;
    NetPeer* peer = &np_peer[p];
    if (!peer->candidates || !peer->token[0]) continue;
    if (peer->direct_ok && now - peer->direct_rx <= NP_DIRECT_QUIET_MS) continue;
    if (now - peer->direct_probe < NP_PUNCH_MS) continue;
    peer->direct_probe = now;
    unsigned char hello[2] = { PKT_HELLO, (uint8_t)np.local_port };
    unsigned char packed[sizeof(RelayHeader) + sizeof hello];
    RelayHeader h = { NP_MAGIC, np.session, (uint16_t)np.local_id, (uint16_t)np.port_ids[p] };
    memcpy(packed, &h, sizeof h);
    memcpy(packed + sizeof h, hello, sizeof hello);
    /* A path already proven is probed at its own address, which is the one
     * the mapping belongs to; an unproven one is probed everywhere. */
    if (peer->direct_ok) direct_send_to(&peer->direct_addr, peer->token, packed, sizeof packed);
    else for (int c = 0; c < peer->candidates; ++c)
      direct_send_to(&peer->candidate[c], peer->token, packed, sizeof packed);
  }
}
static void udp_service(void) {
  if (np.udp == INVALID_SOCKET) return;
  DWORD now = GetTickCount();
  for (int budget = 0; budget < 64; ++budget) {
    unsigned char datagram[1500];
    struct sockaddr_in from; socklen_t from_len = sizeof from;
    int n = (int)recvfrom(np.udp, (char*)datagram, (int)sizeof datagram, 0, (struct sockaddr*)&from, &from_len);
    if (n <= 0) break;
    /* Our own token in front means a player sent this straight to us. The
     * token is what decides, never the source address: a NAT gives no warning
     * before renumbering a mapping, and an address alone proves nothing. */
    if (np.allow_direct && np.peer_token[0] && n >= (int)(NP_UDP_TOKEN + sizeof(RelayHeader))
        && !memcmp(datagram, np.peer_token, NP_UDP_TOKEN)) {
      RelayHeader h; memcpy(&h, datagram + NP_UDP_TOKEN, sizeof h);
      if (h.magic != NP_MAGIC || h.session != np.session || !np.session) continue;
      int port = port_of_id(h.from);
      if (port < 0 || port == np.local_port) continue;
      direct_received(port, &from, now);
      np.last_rx = now;
      handle_relay(datagram + NP_UDP_TOKEN, n - (int)NP_UDP_TOKEN);
      continue;
    }
    if (from.sin_addr.s_addr != np.udp_addr.sin_addr.s_addr) continue;
    np.udp_last_rx = now;
    if (!np.udp_ready) {
      np.udp_ready = 1;
      lock(); np.ui.udp_active = 1; unlock();
      fprintf(stderr, "[netplay] datagram channel confirmed; inputs no longer queue behind the stream\n");
    }
    handle_relay(datagram, n);
  }
  if (!np.session) return;
  /* A direct path that stopped answering is dropped rather than waited on.
   * The relay never went away, so falling back to it costs the hop and
   * nothing else. */
  for (int p = 0; p < 4; ++p) {
    NetPeer* peer = &np_peer[p];
    if (!peer->direct_ok || now - peer->direct_rx <= NP_DIRECT_QUIET_MS) continue;
    peer->direct_ok = 0;
    lock(); if (np.direct_peers) --np.direct_peers; np.ui.direct_peers = np.direct_peers; unlock();
    fprintf(stderr, "[netplay] direct path to port %d went quiet; back through the server\n", p);
  }
  punch_peers(now);
  if (now - np.udp_last_probe >= NP_UDP_PROBE_MS) {
    np.udp_last_probe = now;
    if (!np.udp_ready) {
      /* Registration doubles as the probe: the server records the address the
       * datagram arrived from and echoes the session's traffic back to it. */
      unsigned char hello[2] = { PKT_HELLO, (uint8_t)np.local_port };
      unsigned char packed[sizeof(RelayHeader) + sizeof hello];
      RelayHeader h = { NP_MAGIC, np.session, (uint16_t)np.local_id, 0 };
      memcpy(packed, &h, sizeof h);
      memcpy(packed + sizeof h, hello, sizeof hello);
      udp_send(packed, sizeof packed);
    }
  }
  if (np.udp_ready && now - np.udp_last_rx > NP_UDP_QUIET_MS) {
    np.udp_ready = 0;
    lock(); np.ui.udp_active = 0; unlock();
    fprintf(stderr, "[netplay] datagram channel went quiet; inputs are back on the stream\n");
  }
}
static void auto_step(void) {
  if (!np.auto_mode) return;
  int phase; lock(); phase = np.ui.phase; unlock();
  if (phase == NETPLAY_PHASE_OFFLINE && !np.connecting) { netplay_post(NETPLAY_CMD_CONNECT, 0, NULL, NULL); return; }
  if (phase == NETPLAY_PHASE_LOBBY) {
    static DWORD last_action; DWORD now = GetTickCount();
    if (now - last_action < 500) return;
    last_action = now;
    if (np.auto_host) { NetplayRules r; lock(); r = np.ui.rules; unlock(); netplay_post(NETPLAY_CMD_CREATE, 0, np.pending_room, &r); }
    else {
      int id = 0;
      lock(); for (int i = 0; i < np.ui.room_count; i++) if (!strcmp(np.ui.rooms[i].name, np.pending_room)) id = np.ui.rooms[i].id; unlock();
      if (id) netplay_post(NETPLAY_CMD_JOIN, id, NULL, NULL); else tcp_send_line("{\"op\":\"list\"}");
    }
  } else if (phase == NETPLAY_PHASE_ROOM) {
    int ready = 0, all, host;
    lock(); all = np.ui.all_ready; host = np.ui.is_host;
    for (int i = 0; i < np.ui.player_count; i++) if (np.ui.players[i].id == np.local_id) ready = np.ui.players[i].ready;
    unlock();
    if (!ready) netplay_post(NETPLAY_CMD_READY, 1, NULL, NULL);
    else if (host && all && !np.auto_started) { netplay_post(NETPLAY_CMD_START, 0, NULL, NULL); np.auto_started = 1; }
  }
}
#ifdef _WIN32
static DWORD WINAPI net_thread(LPVOID unused) {
#else
static DWORD net_thread(LPVOID unused) {
#endif
  (void)unused;
  while (!np.quit) {
    process_command();
    profile_send();
    nm_poll();
    if (nm.connect_pending && nm.compat_ready) start_connect();
    nm_pending_command();
    if (np.connected && !nm_aspect_valid()) disconnect("Aspect setting changed. Reconnect to Online.");
    auto_step();
    DWORD now = GetTickCount();
    if (np.connecting) {
      fd_set w, e; FD_ZERO(&w); FD_ZERO(&e); FD_SET(np.tcp, &w); FD_SET(np.tcp, &e);
      struct timeval tv = {0, 0};
      int r = select((int)np.tcp + 1, NULL, &w, &e, &tv);
      if (r > 0 && FD_ISSET(np.tcp, &w)) finish_connect();
      else if ((r > 0 && FD_ISSET(np.tcp, &e)) || now - np.connect_started > 8000) link_lost("Could not reach the server");
    }
    if (np.connected) {
      if (!np.upgrading && !nm.hello_sent) send_hello();
      if (!tcp_flush()) { link_lost("Could not send to the server. Please reconnect."); continue; }
      char buffer[4096]; int n = np_recv(np.tcp, buffer, sizeof buffer);
      if (n > 0) {
        if (np.rx_len + (unsigned)n < sizeof np.rx) { memcpy(np.rx + np.rx_len, buffer, (size_t)n); np.rx_len += (unsigned)n; }
        else { link_lost("Server response exceeded the supported size"); continue; }
        if (np.upgrading) {
          /* Wait for the end of the response headers, then start the protocol. */
          np.rx[np.rx_len < sizeof np.rx ? np.rx_len : sizeof np.rx - 1] = 0;
          char* body = strstr(np.rx, "\r\n\r\n");
          unsigned skip = 4;
          if (!body) { body = strstr(np.rx, "\n\n"); skip = 2; }
          if (!body) continue;
          if (!strstr(np.rx, " 101")) { link_lost("The server did not accept the connection"); continue; }
          unsigned used = (unsigned)(body - np.rx) + skip;
          memmove(np.rx, np.rx + used, np.rx_len - used); np.rx_len -= used;
          np.upgrading = 0;
          send_hello();
        }
        for (;;) {
          char* nl = memchr(np.rx, '\n', np.rx_len);
          if (!nl) break;
          /* Consume first: protocol rejection may close the connection and
           * reset rx_len inside handle_line. Subtracting afterwards underflows. */
          char line[65536]; unsigned line_len = (unsigned)(nl - np.rx);
          memcpy(line, np.rx, line_len); line[line_len] = 0;
          unsigned used = line_len + 1;
          memmove(np.rx, np.rx + used, np.rx_len - used); np.rx_len -= used;
          handle_line(line);
          if (!np.connected) break;
        }
      } else if (n == 0 || (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) link_lost("Server connection closed");
      /* The relay sizes the automatic delay from these. A player can create
       * a room and start it within a second of connecting, so the first
       * measurement is taken immediately and reported the moment it lands,
       * rather than waiting for the next heartbeat. The message is a few
       * dozen bytes; the heartbeat that follows keeps it current. */
      if (np.connected && nm.hello_sent) {
        int carries_measurement = np.ui.ping_ms >= 0 && !np.ping_reported && !np.active;
        if (carries_measurement || now - np.last_ping > 2000) {
          char ping[160];
          /* The private address rides along with the heartbeat rather than
           * getting a message of its own: the server needs it before a room
           * starts, and this is already the thing that arrives before one. */
          char local[80] = "";
          if (np.allow_direct && np.local_udp[0])
            snprintf(local, sizeof local, ",\"local\":\"%s\"", np.local_udp);
          if (np.ui.ping_ms >= 0 && !np.active) {
            snprintf(ping, sizeof ping, "{\"op\":\"ping\",\"rtt\":%d%s}", np.ui.ping_ms, local);
            np.ping_reported = 1;
          } else snprintf(ping, sizeof ping, "{\"op\":\"ping\"%s}", local);
          np.last_ping = now; tcp_send_line(ping);
        }
      }
    }
    udp_service();
    if (np.active) ping_peers();
    if (np.armed && !np.active) {
      if (now - np.last_hello > 200) { np.last_hello = now; unsigned char hello[2] = { PKT_HELLO, (uint8_t)np.local_port }; relay_send(hello, sizeof hello, 0); }
      int all = 1; for (int p = 0; p < 4; p++) if (np.port_ids[p] && p != np.local_port && !np.peer_seen[p]) all = 0;
      if (all) { InterlockedExchange(&np.active, 1); InterlockedExchange(&np.start_pending, 1); status("Players connected. Starting..."); }
      else if (GetTickCount() - np.last_rx > NP_ARM_TIMEOUT_MS) end_session("Could not reach the other players");
    }
    /* Peer silence during a match is an interruption, never a verdict: the
     * grace period in the scene loop is what decides, and it resumes the
     * instant anything arrives. */
    if (np.active && GetTickCount() - np.last_hello > 500) {
      np.last_hello = GetTickCount();
      unsigned char hello[2] = { PKT_HELLO, (uint8_t)np.local_port };
      relay_send(hello, sizeof hello, 0);
    }
    /* handle_relay updates last_rx during this iteration; using the earlier
     * `now` underflows DWORD subtraction and falsely disconnects a live peer. */
    if (np.active && GetTickCount() - np.last_rx > NP_PEER_TIMEOUT_MS) end_session("Connection lost");
    /* A peer that said goodbye and never came back has really gone. */
    if (np.active) for (int p = 0; p < 4; ++p)
      if (np_peer[p].gone && GetTickCount() - np_peer[p].gone_at > NP_BYE_GRACE_MS) { end_session("Opponent left"); break; }
    /* Rebuild a lobby connection that dropped under a live match. The session
     * id, the rollback window and the simulation are all still here, so the
     * only thing to restore is the socket; the players see the hold and then
     * the match continues from the frame it stopped on. */
    if (np.resuming) {
      DWORD held = GetTickCount() - np.resume_since;
      /* Once a connection is up, the welcome handler sends `resume` and the
       * server's reply is what clears this; nothing to do here but wait. */
      int reconnected = np.connected && nm.hello_sent;
      if (reconnected) { /* waiting on the server's resume reply */ }
      else if (held > NP_RECONNECT_GRACE_MS) { np.resuming = 0; end_session("Lost the connection to the server"); }
      /* Only start a fresh attempt when there is none in flight: a socket
       * that is connected but still upgrading must be left to finish. */
      else if (!np.connecting && !np.connected && GetTickCount() - np.reconnect_at >= NP_RECONNECT_BACKOFF_MS) {
        np.reconnect_at = GetTickCount();
        lock(); np.ui.reconnecting = 1; unlock();
        np_interrupt("Reconnecting to the server");
        start_connect();
      }
    }
    /* One millisecond of granularity: an input handed over by the guest
     * thread leaves on this pass rather than up to a frame later, and an
     * arriving packet is picked up as soon as the socket has it. Waiting on
     * the socket rather than sleeping is what keeps that cheap. */
    if (np.tcp != INVALID_SOCKET) {
      fd_set readable; FD_ZERO(&readable); FD_SET(np.tcp, &readable);
      int highest = (int)np.tcp;
      if (np.udp != INVALID_SOCKET) { FD_SET(np.udp, &readable); if ((int)np.udp > highest) highest = (int)np.udp; }
      struct timeval tv = { 0, 1000 };
      select(highest + 1, &readable, NULL, NULL, &tv);
    } else Sleep(4);
  }
  return 0;
}

/* ---- guest-thread side ------------------------------------------------------- */
static uint32_t call_guest(Context* ctx, uint32_t fn, uint32_t r3) {
  RecFn f = lookup_function(fn); if (lookup_is_stub(f)) return 0;
  Context saved = *ctx; ctx->gpr[3] = r3; f(ctx); uint32_t r = ctx->gpr[3]; uint64_t tb = ctx->timebase; *ctx = saved; ctx->timebase = tb; return r;
}
static void reseed(Context* ctx) {
  uint32_t seed_ptr = mem_read32(ctx, RNG_SEED_PTR);
  if (valid_guest(seed_ptr, 4)) mem_write32(ctx, seed_ptr, np.seed ^ ((uint32_t)np.epoch * 0x9E3779B9u) ^ 0x2545F491u);
}
/* Original VS entry copies prior offline selections into CSS. Reset that
 * session baseline through gm_InitVsMode, preserving the offline selections. */
static int netplay_prepare_vs(Context* ctx) {
  uint32_t vs = call_guest(ctx, 0x801A5244u, 0); /* gmVsMelee_GetVsData */
  if (!valid_guest(vs, sizeof np.backup_vs)) return 0;
  if (!np.vs_backed) {
    memcpy(np.backup_vs, ctx->ram + (vs & RAM_MASK), sizeof np.backup_vs);
    np.vs_backed = 1;
  }
  call_guest(ctx, 0x80167B50u, vs); /* gm_InitVsMode */
  return 1;
}
static void netplay_restore_vs(Context* ctx) {
  if (!np.vs_backed) return;
  uint32_t vs = call_guest(ctx, 0x801A5244u, 0);
  if (valid_guest(vs, sizeof np.backup_vs)) memcpy(ctx->ram + (vs & RAM_MASK), np.backup_vs, sizeof np.backup_vs);
  np.vs_backed = 0;
}
static void apply_rules(Context* ctx) {
  NetplayRules r; lock(); r = np.ui.rules; unlock();
  uint32_t rules = call_guest(ctx, FN_GAME_RULES, 0), prefs = call_guest(ctx, FN_PREFS, 0);
  if (!valid_guest(rules, 0x18) || !valid_guest(prefs, 0x20)) return;
  if (!np.rules_backed) { memcpy(np.backup_rules, ctx->ram + (rules & RAM_MASK), 0x18); memcpy(np.backup_prefs, ctx->ram + (prefs & RAM_MASK), 0x20); np.rules_backed = 1; }
  mem_write8(ctx, rules + 2, (uint8_t)r.mode);
  mem_write8(ctx, rules + 3, (uint8_t)(r.mode == 0 ? (r.minutes ? r.minutes : 2) : 0));
  mem_write8(ctx, rules + 4, (uint8_t)r.stock);
  mem_write8(ctx, rules + 5, 0);
  mem_write8(ctx, rules + 6, (uint8_t)(r.damage / 10));
  mem_write8(ctx, rules + 8, (uint8_t)(r.mode == 1 ? r.minutes : 0));
  mem_write8(ctx, rules + 9, (uint8_t)r.friendly_fire);
  mem_write8(ctx, rules + 0xA, (uint8_t)r.pause);
  mem_write8(ctx, prefs + 0, (uint8_t)(int8_t)(r.items - 1));
  mem_write64(ctx, prefs + 8, r.items ? 0xFFFFFFFFFFFFFFFFull : 0);
  mem_write32(ctx, prefs + 0x18, 0x1FFFFFFFu);
  fprintf(stderr, "[netplay] rules applied: mode=%d stock=%d minutes=%d items=%d delay=%d\n", r.mode, r.stock, r.minutes, r.items, r.delay);
}
static void restore_rules(Context* ctx) {
  netplay_restore_vs(ctx);
  if (!np.rules_backed) return;
  uint32_t rules = call_guest(ctx, FN_GAME_RULES, 0), prefs = call_guest(ctx, FN_PREFS, 0);
  if (valid_guest(rules, 0x18)) memcpy(ctx->ram + (rules & RAM_MASK), np.backup_rules, 0x18);
  if (valid_guest(prefs, 0x20)) memcpy(ctx->ram + (prefs & RAM_MASK), np.backup_prefs, 0x20);
  np.rules_backed = 0;
}
/* Retain compact, non-personal evidence for a confirmed mismatch. Both players'
 * normal logs then identify the first differing RNG/fighter/input fields. */
static const uint16_t state_fields[] = { 0x10, 0x14, 0x2C, 0x80, 0x84, 0x88, 0x8C,
  0xB0, 0xB4, 0xE0, 0xEC, 0x620, 0x624, 0x65C, 0x668, 0x894, 0x89C, 0x1830 };
#define NP_STATE_FIELDS (sizeof state_fields / sizeof state_fields[0])
static struct { int epoch; uint32_t frame, seed, present, kind[4], fields[4][NP_STATE_FIELDS]; } state_traces[256];
static void state_trace_dump(uint32_t frame) {
  unsigned slot=frame&255;
  if(state_traces[slot].epoch!=np.epoch||state_traces[slot].frame!=frame)return;
  fprintf(stderr,"[netplay-desync] epoch=%d frame=%u rng=%08X present=%X\n",np.epoch,frame,state_traces[slot].seed,state_traces[slot].present);
  for(unsigned p=0;p<4;++p) {
    const NetInput* input=get_input(p,np.epoch,frame);
    if(input)fprintf(stderr,"[netplay-desync] P%u input=%04X stick=%d,%d c=%d,%d triggers=%u,%u\n",p,input->buttons,input->sx,input->sy,input->cx,input->cy,input->lt,input->rt);
    if(!(state_traces[slot].present&(1u<<p)))continue;
    fprintf(stderr,"[netplay-desync] P%u kind=%u",p,state_traces[slot].kind[p]);
    for(unsigned i=0;i<NP_STATE_FIELDS;++i)fprintf(stderr," %03X=%08X",state_fields[i],state_traces[slot].fields[p][i]);
    fprintf(stderr,"\n");
  }
}
static uint32_t state_hash(Context* ctx, uint32_t frame) {
  unsigned slot=frame&255;
  memset(&state_traces[slot],0,sizeof state_traces[slot]);
  state_traces[slot].epoch=np.epoch;state_traces[slot].frame=frame;
  costume_art_frame_done(ctx); /* draw-done already released the FIFO */
  uint32_t h = 2166136261u;
  uint32_t seed_ptr = mem_read32(ctx, RNG_SEED_PTR);
  if (valid_guest(seed_ptr, 4)) { state_traces[slot].seed=mem_read32(ctx, seed_ptr); h = fnv(h, state_traces[slot].seed); }
  for (uint32_t port = 0; port < 4; port++) {
    uint32_t gobj = call_guest(ctx, FN_PLAYER_ENTITY, port);
    if (!valid_guest(gobj, 0x30)) continue;
    uint32_t fp = mem_read32(ctx, gobj + 0x2C);
    if (!valid_guest(fp, 0x2400)) continue;
    /* Include future-affecting motion and controller state, not only the
     * visible position/damage that can agree for a frame after divergence. */
    state_traces[slot].present|=1u<<port;state_traces[slot].kind[port]=mem_read32(ctx,fp+4);
    for (unsigned i = 0; i < NP_STATE_FIELDS; ++i) {
      uint32_t value=mem_read32(ctx,fp+state_fields[i]);
      state_traces[slot].fields[port][i]=value;h=fnv(h,value);
    }
  }
  return h;
}
static void scripted_local(NetInput* in) {
  /* MELEE_NETPLAY_INPUT = "epoch:frame:buttons(hex):length:x:y,..." keyed on session frames. */
  if (!np.have_script) return;
  const char* s = np.script;
  memset(in, 0, sizeof *in);
  while (*s) {
    char* next; long epoch = strtol(s, &next, 10); if (*next != ':') break;
    long first = strtol(next + 1, &next, 10); if (*next != ':') break;
    unsigned buttons = (unsigned)strtoul(next + 1, &next, 16); if (*next != ':') break;
    long length = strtol(next + 1, &next, 10); long x = 0, y = 0;
    if (*next == ':') x = strtol(next + 1, &next, 10);
    if (*next == ':') y = strtol(next + 1, &next, 10);
    if (epoch == np.epoch && (long)np.frame >= first && (long)np.frame < first + length) { in->buttons |= (uint16_t)buttons; in->sx = (int8_t)x; in->sy = (int8_t)y; }
    s = (*next == ',') ? next + 1 : next;
    if (*next != ',') break;
  }
}
static void epoch_begin(Context* ctx, const char* what) {
  lock();
  np.epoch++; np.frame = 0; np.scene_epoch_ready = 1; np.local_merge_valid = 0; memset(&np.local_merge, 0, sizeof np.local_merge);
  np.hash_valid_from = 0; np.stall_started = 0;
  /* Frames restart at zero here, so the clock comparison starts over with
   * them; the measured link carries across unchanged. */
  np.skip_frames = 0;
  for (int p = 0; p < 4; ++p) { np_peer[p].last_frame = 0; np_peer[p].advantage = np_peer[p].remote_advantage = 0; timesync_new_epoch(&np_peer[p].sync); }
  np.ui.epoch = np.epoch; np.ui.frame = 0; np.ui.clock_skips = 0; np.ui.frame_advantage = 0;
  rollback_match = !strcmp(what, "match") || !strcmp(what, "sudden death");
  unlock();
  extern void aurora_link_clear_pad_events(void); aurora_link_clear_pad_events();
  reseed(ctx);
  fprintf(stderr, "[netplay] epoch %d begins at %s\n", np.epoch, what);
}

void netplay_capture_pad(unsigned port, const AushimPadStatus* pad) {
  if (port != 0 || !np.active || rollback_replay) return;
  np.local_merge.buttons |= pad->buttons;
  np.local_merge.sx = pad->stick_x; np.local_merge.sy = pad->stick_y; np.local_merge.cx = pad->cstick_x; np.local_merge.cy = pad->cstick_y;
  if (pad->trigger_left > np.local_merge.lt) np.local_merge.lt = pad->trigger_left;
  if (pad->trigger_right > np.local_merge.rt) np.local_merge.rt = pad->trigger_right;
  np.local_merge_valid = 1;
}
int netplay_lobby_open(void) { return 0; }   /* the lobby is an in-game menu now */
int netplay_costume_activation_allowed(void) { return !np.active && !np.armed && !rollback_running; }
int netplay_session_active(void) { return np.active || np.armed; }
/* Local memory-card tags are not synchronized. Hide them only during online
 * sessions and block NAME ENTRY; the user's saved name bank stays untouched. */
/* Local save progression can request different challenger/prize scenes on
 * each machine after results. Use the original function's terminal-following-
 * state branch to retain KO counts, selections, normal records and card save,
 * while deferring those offline-only checks. The descriptor is restored before
 * returning and the original routine is untouched for offline matches.
 * Reference: gmVsMelee_ExitResults, GameModeState size 0x18. */
void netplay_results_exit(Context* ctx) {
  uint32_t next=ctx->gpr[3]+0x18u;
  if(!np.active || !np.guest_started || !valid_guest(next,1)) { func_801A5F64(ctx); return; }
  uint8_t id=mem_read8(ctx,next);
  mem_write8(ctx,next,0xFF);
  func_801A5F64(ctx);
  mem_write8(ctx,next,id);
  fprintf(stderr,"[netplay] results retained online rematch path\n");
}

void netplay_name_text(Context* ctx) {
  if (netplay_session_active()) ctx->gpr[3] = 0;
  else func_8023754C(ctx);
}
void netplay_name_list_full(Context* ctx) {
  if (netplay_session_active()) ctx->gpr[3] = 1;
  else func_802375EC(ctx);
}
static void netplay_clear_css_names(Context* ctx) {
  /* Original CSSData.vs.start.players: first nametag +0x7A, stride0x24.
   * Clear scene-local selections, never persistent NameTagData.namedata. */
  uint32_t css = ctx->gpr[3];
  if (valid_guest(css, 0x100))
    for (unsigned port = 0; port < 4; ++port) mem_write8(ctx, css + 0x7A + port * 0x24, 0x78);
}


/* Thread-safe copy of the lobby state for the in-game menu. */
void netplay_snapshot(MeleeNetplayUi* out) { lock(); *out = np.ui; unlock(); }

/* Queue one lobby command from the menu. Returns 0 when one is still pending. */
int netplay_post(int cmd, int arg, const char* text, const NetplayRules* rules) {
  int accepted = 0;
  lock();
  if (np.ui.cmd == NETPLAY_CMD_NONE) {
    np.ui.cmd_arg = arg;
    if (text) snprintf(np.ui.cmd_text, sizeof np.ui.cmd_text, "%s", text);
    if (rules) np.ui.cmd_rules = *rules;
    if (cmd == NETPLAY_CMD_CONNECT) {
      snprintf(np.ui.edit_server, sizeof np.ui.edit_server, "%s", np.ui.server);
      snprintf(np.ui.edit_name, sizeof np.ui.edit_name, "%s", np.ui.name);
    }
    np.ui.cmd = cmd;
    accepted = 1;
  }
  unlock();
  return accepted;
}

/* Called once per menu frame from the guest thread. */
/* Called when the player opens the netplay screen. */
void netplay_menu_opened(void) {
  if (np.connected || np.connecting) return;
  netplay_post(NETPLAY_CMD_CONNECT, 0, NULL, NULL);
}

void netplay_menu_tick(Context* ctx, int* start_request) {
  *start_request = 0;
  if (InterlockedExchange(&np.start_pending, 0)) {
    if (costumes_active_ready() <= 0) {
      end_session("Costume verification changed before the match. Session canceled.");
      tcp_send_line("{\"op\":\"leave\"}");
      return;
    }
    if (!netplay_prepare_vs(ctx)) {
      end_session("Could not prepare matching character-select state.");
      tcp_send_line("{\"op\":\"leave\"}");
      return;
    }
    apply_rules(ctx);
    lock(); np.ui.phase = NETPLAY_PHASE_PLAYING; np.ui.delay = np.delay; unlock();
    np.epoch = 0; np.frame = 0;
    InterlockedExchange(&np.guest_started, 1);
    *start_request = 1;
    { int peer = np_peer_ping(); char link[32];
      if (peer >= 0) snprintf(link, sizeof link, "%d ms", peer); else snprintf(link, sizeof link, "measuring");
      fprintf(stderr, "[netplay] session %u started as port %d, delay %d%s, server ping %d ms, peer %s, transport %s\n",
              np.session, np.local_port + 1, np.delay, np.ui.rules.delay ? "" : " (automatic)",
              np.ui.ping_ms, link, np.udp_ready ? "datagram" : "stream"); }
  }
}

void netplay_scene_event(Context* ctx, uint32_t addr) {
  if (addr == 0x8022DDA8u || addr == 0x801A1E20u) {      /* main menu / title */
    /* Auto-connect can finish during title loading. The first menu entry must
     * consume start_pending before any in-game scene has begun. */
    if (np.active && np.epoch > 0) end_session("Returned to the menu");
    if (!np.active) restore_rules(ctx);
    return;
  }
  /* Peer connectivity can finish during title/attract-mode loading. These
   * unrelated offline scene callbacks must never become online epochs. */
  if (!np.active || !np.guest_started) return;
  if (np.epoch == 0 && addr != 0x8026688Cu) return;
  switch (addr) {
  case 0x8026688Cu: netplay_clear_css_names(ctx); epoch_begin(ctx, "character select"); break;
  case 0x8025A998u: epoch_begin(ctx, "stage select"); break;
  case 0x8016E934u: epoch_begin(ctx, "match"); break;
  case 0x8016EBC0u: epoch_begin(ctx, "sudden death"); break;
  case 0x80177368u: epoch_begin(ctx, "results"); break;
  }
}

extern void controller_online_queue(Context*);
/* HSD_PadRenewMasterStatus: one synchronised game frame. */
void netplay_master_status(Context* ctx) {
  if (rollback_step) {
    uint32_t qread = mem_read8(ctx, PADLIB + 1), queue = mem_read32(ctx, PADLIB + 8);
    uint32_t entry = queue + qread * 48;
    if (valid_guest(entry, 48)) {
      mem_write8(ctx, PADLIB + 3, 1);
      mem_write8(ctx, PADLIB + 2, (uint8_t)((qread + 1) % (mem_read8(ctx, PADLIB) ? mem_read8(ctx, PADLIB) : 1)));
      for (unsigned p = 0; p < 4; ++p) {
        uint32_t at = entry + p * 12; const RbInput* in = &rollback_pads[p];
        mem_write16(ctx, at, in->buttons); mem_write8(ctx, at + 2, (uint8_t)in->sx); mem_write8(ctx, at + 3, (uint8_t)in->sy);
        mem_write8(ctx, at + 4, (uint8_t)in->cx); mem_write8(ctx, at + 5, (uint8_t)in->cy);
        mem_write8(ctx, at + 6, in->lt); mem_write8(ctx, at + 7, in->rt);
        mem_write8(ctx, at + 8, 0); mem_write8(ctx, at + 9, 0);
        mem_write8(ctx, at + 10, np.port_ids[p] ? 0 : 255); mem_write8(ctx, at + 11, 0);
      }
    }
    controller_online_queue(ctx); func_8037750C(ctx); return;
  }
  if (!np.active || np.epoch == 0) { controller_online_queue(ctx); func_8037750C(ctx); return; }
  uint32_t f = np.frame; int epoch = np.epoch;
  NetInput local = np.local_merge; if (np.have_script) scripted_local(&local);
  np.local_merge_valid = 0; memset(&np.local_merge, 0, sizeof np.local_merge);
  lock(); store_input(np.local_port, epoch, f + (uint32_t)np.delay, &local); unlock();
  send_inputs(f + (uint32_t)np.delay, NP_REDUNDANT);
  DWORD wait_started = GetTickCount(), last_request = wait_started; int stalled = 0;
  for (;;) {
    int ready = 1;
    lock();
    for (int p = 0; p < 4; p++) if (np.port_ids[p] && p != np.local_port && f >= (uint32_t)np.delay && !get_input(p, epoch, f)) ready = 0;
    unlock();
    if (ready || !np.active || np.quit) break;
    DWORD now = GetTickCount(), waited = now - wait_started;
    /* Held, not ended: the menu keeps its place and resumes where it was. */
    if (waited > NP_STALL_GRACE_MS) { end_session("Opponent stopped responding"); break; }
    if (now - last_request > 40) { last_request = now; unsigned char req[5] = { PKT_REQUEST }; memcpy(req + 1, &f, 4); relay_send(req, sizeof req, 0); send_inputs(f + (uint32_t)np.delay, NP_REDUNDANT); }
    if (!stalled) { stalled = 1; np.stall_started = now; }
    if (waited > NP_STALL_NOTICE_MS) np_interrupt("Waiting for opponent");
    lock(); np.ui.stalled_ms = (int)waited; np.ui.interrupt_ms = (int)np_interrupt_age(); unlock();
    recomp_poll(ctx);
    Sleep(1);
  }
  if (stalled) { frontend_drop_frame_backlog(); lock(); np.ui.stalled_ms = 0; unlock(); np_resume(); }
  if (!np.active) { controller_online_queue(ctx); func_8037750C(ctx); return; }
  /* Publish the agreed inputs as the raw queue entry the original will consume. */
  uint32_t qread = mem_read8(ctx, PADLIB + 1), queue = mem_read32(ctx, PADLIB + 8);
  if (mem_read8(ctx, PADLIB + 3) == 0) { mem_write8(ctx, PADLIB + 3, 1); mem_write8(ctx, PADLIB + 2, (uint8_t)((qread + 1) % (mem_read8(ctx, PADLIB) ? mem_read8(ctx, PADLIB) : 1))); }
  uint32_t entry = queue + qread * 48;
  if (valid_guest(entry, 48)) {
    lock();
    for (int p = 0; p < 4; p++) {
      uint32_t at = entry + (uint32_t)p * 12; NetInput zero = {0}; const NetInput* in = NULL;
      if (np.port_ids[p]) in = f >= (uint32_t)np.delay ? get_input(p, epoch, f) : &zero;
      if (!in) in = &zero;
      mem_write16(ctx, at, in->buttons); mem_write8(ctx, at + 2, (uint8_t)in->sx); mem_write8(ctx, at + 3, (uint8_t)in->sy);
      mem_write8(ctx, at + 4, (uint8_t)in->cx); mem_write8(ctx, at + 5, (uint8_t)in->cy); mem_write8(ctx, at + 6, in->lt); mem_write8(ctx, at + 7, in->rt);
      mem_write8(ctx, at + 8, 0); mem_write8(ctx, at + 9, 0); mem_write8(ctx, at + 10, np.port_ids[p] ? 0 : (uint8_t)-1); mem_write8(ctx, at + 11, 0);
    }
    unlock();
  }
  uint32_t h = state_hash(ctx, f);
  lock();
  np.hashes[f & 255] = h; np.frame = f + 1; np.ui.frame = (int)np.frame;
  for (int p = 0; p < 4; p++) if (np.remote_hash_new[p]) {
    np.remote_hash_new[p] = 0;
    uint32_t rf = np.remote_hash_frame[p];
    if (np.remote_hash_epoch[p] == (uint8_t)epoch && rf < np.frame && np.frame - rf < 256 && np.hashes[rf & 255] != np.remote_hash[p] && !np.desynced) {
      /* Recorded and shown, never acted on. A mismatch means the two games
       * have stopped agreeing, but ending the match there and then throws
       * away a game that is usually still perfectly playable, and the players
       * are better placed than we are to decide whether it still counts. */
      InterlockedExchange(&np.desynced, 1); np.ui.desynced = 1;
      fprintf(stderr, "[netplay] DESYNC at epoch %d frame %u: local %08X remote %08X\n", epoch, rf, np.hashes[rf & 255], np.remote_hash[p]);
      state_trace_dump(rf);
    }
  }
  unlock();
  if ((f % 60) == 0) fprintf(stderr, "[netplay] epoch %d frame %u hash %08X\n", epoch, f, h);
  controller_online_queue(ctx); func_8037750C(ctx);
}

/* One simulated frame of clock bookkeeping, called by the scene loop after
 * the frame is committed.
 *
 * Each peer's pair of measurements is recorded every frame, but a correction
 * is only considered every NP_SYNC_INTERVAL frames: the decision is made from
 * half a second of averaged evidence, so a single late packet cannot cause a
 * hitch, and applying it no more often than the evidence refreshes stops the
 * same drift being paid for twice. Corrections are large only while the match
 * is settling; after that they are one frame at a time, below perception. */
static void netplay_clock_tick(void) {
  if (!np.active) return;
  int advantage = 0, have = 0, skip = 0;
  uint32_t frame;
  lock();
  frame = np.frame;
  for (int p = 0; p < 4; ++p) {
    if (!np.port_ids[p] || p == np.local_port) continue;
    NetPeer* peer = &np_peer[p];
    int value = timesync_advantage(&peer->sync, frame, peer->last_frame);
    peer->advantage = value;
    timesync_frame(&peer->sync, value, peer->remote_advantage);
    if (!have || value < advantage) { advantage = value; have = 1; }
  }
  np.ui.frame_advantage = have ? advantage : 0;
  np.ui.rollbacks = (int)rollback.rollback_count;
  np.ui.rollback_frames = (int)rollback.replayed_frames;
  np.ui.udp_active = np.udp_ready;
  { int ping = np_peer_ping(); if (ping >= 0) np.ui.ping_ms = ping; }
  if (have && frame % NP_SYNC_INTERVAL == 0) {
    /* Converge quickly while the match is settling, then stay imperceptible. */
    int most = frame <= 120 ? TS_MAX_SKIP : 1;
    for (int p = 0; p < 4; ++p) {
      if (!np.port_ids[p] || p == np.local_port) continue;
      int value = timesync_skip_frames(&np_peer[p].sync, most);
      if (value > skip) skip = value;
    }
  }
  unlock();
  /* A periodic line so a connection can be judged from the log as well as
   * from the on-screen readout: what the link measures, how far apart the
   * two clocks are, and how much rollback that is costing. */
  if (frame && frame % 300 == 0) {
    lock();
    int ping = np.ui.ping_ms, held = np.ui.clock_skips, udp = np.ui.udp_active;
    int direct = np.ui.direct_peers;
    unsigned corrections = rollback.rollback_count, replayed = rollback.replayed_frames;
    unlock();
    fprintf(stderr, "[netplay] link: peer %d ms, advantage %+d, corrections %u (%u frames replayed),"
                    " clock holds %d, transport %s\n",
            ping, advantage, corrections, replayed, held,
            direct ? "direct" : udp ? "relayed datagram" : "relayed stream");
  }
  if (skip > 0) {
    np.skip_frames = skip;
    fprintf(stderr, "[netplay] clock: giving back %d frame(s) at frame %u (advantage %d)\n",
            skip, frame, advantage);
  }
}

#include "netplay_rollback.inc"

void netplay_init(void) {
#ifdef _WIN32
  WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  InitializeCriticalSection(&np.lock); InitializeCriticalSection(&np.send_lock);
  np.tcp = INVALID_SOCKET;
  np.ui.size = sizeof np.ui; np.ui.version = NETPLAY_UI_VERSION; np.ui.phase = NETPLAY_PHASE_OFFLINE; np.ui.ping_ms = -1;
  rules_default(&np.ui.rules);
  aurora_link_netplay_settings(np.ui.server, sizeof np.ui.server, np.ui.name, sizeof np.ui.name);
  const char* env = getenv("MELEE_NETPLAY_SERVER"); if (env && *env) snprintf(np.ui.server, sizeof np.ui.server, "%s", env);
  env = getenv("MELEE_NETPLAY_NAME"); if (env && *env) snprintf(np.ui.name, sizeof np.ui.name, "%s", env);
  if (!np.ui.name[0]) snprintf(np.ui.name, sizeof np.ui.name, "Player");
  /* The in-game screens cannot take typed text, so a usable address must always
     exist. Settings written before there was a public server point at a server
     on this machine, which no longer exists, so they are moved across too. */
  if (!np.ui.server[0] || !strcmp(np.ui.server, "127.0.0.1:7420") || !strcmp(np.ui.server, "localhost:7420"))
    snprintf(np.ui.server, sizeof np.ui.server, "%s", official_server());
  /* Direct paths are the default; MELEE_NETPLAY_RELAY_ONLY=1 keeps every
   * packet on the server. */
  { const char* relay_only = getenv("MELEE_NETPLAY_RELAY_ONLY");
    np.allow_direct = !(relay_only && relay_only[0] == '1');
    if (!np.allow_direct) fprintf(stderr, "[netplay] direct connections disabled; matches stay on the server\n"); }
  env = getenv("MELEE_NETPLAY_AUTO");
  if (env && (!strncmp(env, "host:", 5) || !strncmp(env, "join:", 5))) { np.auto_mode = 1; np.auto_host = env[0] == 'h'; snprintf(np.pending_room, sizeof np.pending_room, "%s", env + 5); }
  env = getenv("MELEE_NETPLAY_RULES");
  if (env && *env) { char text[256]; snprintf(text, sizeof text, "{%s}", env); rules_from_json(text, &np.ui.rules); }
  env = getenv("MELEE_NETPLAY_INPUT"); if (env && *env) { snprintf(np.script, sizeof np.script, "%s", env); np.have_script = 1; }
  snprintf(np.ui.status, sizeof np.ui.status, "Not connected");
  aurora_link_netplay_bind(&np.ui);
  nm_request_compatibility();
  env=getenv("MELEE_CONTENT_JOIN_ROOM");
  if(env&&atoi(env)>0){nm.queued_cmd=NETPLAY_CMD_JOIN;nm.queued_arg=atoi(env);}
#ifdef _WIN32
  np.thread = CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
#else
  np.thread = CreateThreadSimple(0, net_thread, NULL);
  np.thread_valid = np.thread != 0;
#endif
  fprintf(stderr, "[netplay] ready; server=%s name=%s%s\n",
          is_official(np.ui.server) ? "official" : np.ui.server, np.ui.name,
          np.auto_mode ? " (automatic test session)" : "");
}
void netplay_shutdown(void) {
#ifdef _WIN32
  if (!np.thread) return;
  InterlockedExchange(&np.quit, 1);
  end_session("Shutdown");
  if(WaitForSingleObject(np.thread,5000)!=WAIT_OBJECT_0){fprintf(stderr,"Online worker did not stop safely\n");ExitProcess(8);}
  CloseHandle(np.thread);np.thread=NULL;
#else
  if (!np.thread_valid) return;
  InterlockedExchange(&np.quit, 1);
  end_session("Shutdown");
  platform_thread_join(np.thread, 2000);
  np.thread_valid = 0;
#endif
  nm_close_worker(1);
  nm_unlock_inputs();
  nm_release_aspect();
  close_sockets();
}

void netplay_release(void){
 rb_destroy(&rollback);
 if(np.ssl_ctx){SSL_CTX_free(np.ssl_ctx);np.ssl_ctx=NULL;}
 DeleteCriticalSection(&np.lock);DeleteCriticalSection(&np.send_lock);
#ifdef _WIN32
 WSACleanup();
#endif
}
