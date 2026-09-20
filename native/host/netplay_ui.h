/* Shared lobby state between the MinGW runtime (netplay.c) and the MSVC
 * renderer DLL (netplay_menu.cpp). Plain C, fixed-size fields only: the struct
 * crosses the DLL boundary by pointer. The runtime owns the struct and updates
 * the state fields; the renderer only writes the command fields. */
#ifndef MELEE_NETPLAY_UI_H
#define MELEE_NETPLAY_UI_H
#include <stdint.h>

#define NETPLAY_UI_VERSION 6u
#define NETPLAY_MAX_MODS 16
#define NETPLAY_MOD_CATALOG 64
#define NETPLAY_MAX_ROOMS 32
#define NETPLAY_MAX_PLAYERS 4

enum {
  NETPLAY_PHASE_OFFLINE = 0,
  NETPLAY_PHASE_CONNECTING,
  NETPLAY_PHASE_LOBBY,
  NETPLAY_PHASE_ROOM,
  NETPLAY_PHASE_STARTING,
  NETPLAY_PHASE_PLAYING,
};

enum {
  NETPLAY_CMD_NONE = 0,
  NETPLAY_CMD_CONNECT,      /* uses edit_server / edit_name */
  NETPLAY_CMD_DISCONNECT,
  NETPLAY_CMD_REFRESH,
  NETPLAY_CMD_CREATE,       /* cmd_text = room name, cmd_rules */
  NETPLAY_CMD_JOIN,         /* cmd_arg = room id */
  NETPLAY_CMD_LEAVE,
  NETPLAY_CMD_READY,        /* cmd_arg = 0/1 */
  NETPLAY_CMD_START,
  NETPLAY_CMD_RULES,        /* cmd_rules (host only) */
  NETPLAY_CMD_CLOSE,        /* hide the lobby, keep the connection */
  NETPLAY_CMD_END_SESSION,  /* leave an active match session */
  NETPLAY_CMD_MOD_REFRESH, /* fetch public Mod Browser catalog */
  NETPLAY_CMD_MOD_PREPARE, /* cmd_arg = catalog index; show install confirmation */
  NETPLAY_CMD_MOD_CONFIRM, /* consent to install pending catalog/room packages */
  NETPLAY_CMD_MOD_CANCEL,  /* dismiss pending download prompt, remain outside room */
  NETPLAY_CMD_MOD_TOGGLE,  /* cmd_arg = catalog index; enable/disable local costume pack */
  NETPLAY_CMD_MOD_INSPECT, /* reserved: former native model inspector */
  NETPLAY_CMD_MOD_INSPECT_COSTUME, /* reserved */
  NETPLAY_CMD_MOD_PREVIEW, /* catalog index; public PNG only, never a package ZIP */
};

enum { NETPLAY_NAME_OPEN = 1, NETPLAY_NAME_APPEND, NETPLAY_NAME_BACKSPACE,
       NETPLAY_NAME_CLEAR, NETPLAY_NAME_CONFIRM, NETPLAY_NAME_CANCEL };

typedef struct NetplayRules {
  int32_t mode;          /* 0 time, 1 stock */
  int32_t stock;         /* 1..99 */
  int32_t minutes;       /* 0 = no limit, 1..99 */
  int32_t items;         /* 0 none, 1 very low .. 5 very high */
  int32_t delay;         /* input delay frames 1..10 */
  int32_t pause;         /* 0/1 */
  int32_t damage;        /* damage ratio percent, 50..200 */
  int32_t friendly_fire; /* 0/1 */
} NetplayRules;

typedef struct NetplayPlayer {
  int32_t id, ready, port;
  char name[32];
  char avatar[65]; /* SHA256 of canonical 96x96 RGBA; never a URL or file path */
} NetplayPlayer;

typedef struct NetplayRoom {
  int32_t id, players, max, state; /* state 0 open, 1 playing */
  char name[48];
  char host[32];
  int32_t rules_known; /* false when summary omits rules metadata */
  NetplayRules rules;
} NetplayRoom;

typedef struct NetplayMod {
  char sha256[65], id[65], name[64], version[24], base[32];
  char description[256];
  int32_t bytes, costumes, installed, enabled;
} NetplayMod;

typedef struct MeleeNetplayUi {
  uint32_t size, version;
  /* runtime -> renderer */
  int32_t phase, open, is_host, local_id, room_id, player_count, room_count;
  int32_t frame, epoch, delay, stalled_ms, desynced, ping_ms, session_active, all_ready;
  char status[160];
  char server[128];
  char name[32];
  char room_name[48];
  NetplayRules rules;
  NetplayPlayer players[NETPLAY_MAX_PLAYERS];
  NetplayRoom rooms[NETPLAY_MAX_ROOMS];
  /* Repository state. Publisher credentials never cross into UI. */
  int32_t mod_catalog_count, mod_catalog_loading, mod_catalog_more;
  int32_t mod_prompt;     /* 0 none, 1 room requirements, 2 catalog install */
  int32_t mod_pending_room, mod_required_count;
  int32_t mod_job;        /* 0 idle, 1 fetching catalog, 2 downloading/installing */
  int32_t mod_progress;   /* 0..100 when known */
  char mod_status[160];
  NetplayMod mod_catalog[NETPLAY_MOD_CATALOG];
  NetplayMod mod_required[NETPLAY_MAX_MODS];
  /* Guest menu camera pose, filled after the network snapshot on the guest thread. */
  int32_t menu_pose_valid;
  float menu_pose[9];
  /* Reserved former model-viewer state; retained for ABI version 5. */
  int32_t mod_preview_ready, mod_preview_color, mod_preview_colors, mod_inspecting;
  char mod_preview_name[64];
  /* Verified static PNG worker data. ready: 0 pending, 1 ready, -1 absent/error.
   * mod_inspect_package is the selected package hash; mod_inspect_sha256 is
   * the PNG hash. Internal paths must never be rendered as UI text. */
  int32_t mod_inspect_loading, mod_inspect_ready, mod_inspect_costumes;
  int32_t mod_inspect_kind, mod_inspect_base;
  char mod_preview_path[1024];
  char mod_inspect_sha256[65], mod_inspect_name[64], mod_inspect_package[65];
  int32_t mod_inspect_color;
  /* renderer -> runtime (renderer sets cmd only while cmd == NETPLAY_CMD_NONE) */
  int32_t cmd, cmd_arg;
  char cmd_text[48];
  NetplayRules cmd_rules;
  char edit_server[128];
  char edit_name[32];
  int32_t room_name_open;
  char room_name_draft[48]; /* printable ASCII, 47 bytes plus terminator */
} MeleeNetplayUi;

#endif
