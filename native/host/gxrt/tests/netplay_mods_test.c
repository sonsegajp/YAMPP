/* Exercises the production native prompt, hash gate, and hidden Python worker.
 * The guest costume loader is substituted; actual-game checks cover that layer. */
#include <assert.h>
#include "../netplay.c"

int g_mex_active;
static int test_costume_ready = 1;
static int test_aspect, test_aspect_locked, test_aspect_unavailable;
int aurora_link_widescreen(int set,int value){if(set&&!test_aspect_locked)test_aspect=!!value;return test_aspect;}
int aurora_link_aspect_lock(int command){if(test_aspect_unavailable)return -1;if(command<0)return test_aspect_locked;test_aspect_locked=!!command;return test_aspect;}

void costumes_set_room_mods(const char* hashes) { (void)hashes; test_costume_ready = 1; }
int costumes_active_ready(void) { return test_costume_ready; }
void mem_write8(Context* ctx, uint32_t address, uint8_t value) { ctx->ram[address & 0x1ffffffu] = value; }
void func_8023754C(Context* ctx) { ctx->gpr[3] = 0x12345678; }
void func_802375EC(Context* ctx) { ctx->gpr[3] = 0; }
int lookup_is_stub(RecFn fn) { return fn == NULL; }
static int original_vs_resets;
static void test_vs_pointer(Context* ctx) { ctx->gpr[3] = 0x80004000; }
static void test_original_vs_init(Context* ctx) {
  assert(ctx->gpr[3] == 0x80004000); ++original_vs_resets;
  memset(ctx->ram + 0x4000, 0xCC, 0x140);
}
RecFn lookup_function(uint32_t address) {
  if (address == 0x801A5244u) return test_vs_pointer;
  assert(address == 0x80167B50u); return test_original_vs_init;
}


int main(int argc, char** argv) {
  InitializeCriticalSection(&np.lock); InitializeCriticalSection(&np.send_lock);
  np.tcp = INVALID_SOCKET;
  assert(netplay_room_name_input(NETPLAY_NAME_OPEN, NULL));
  assert(np.ui.room_name_open && !np.ui.room_name_draft[0]);
  assert(netplay_room_name_input(NETPLAY_NAME_APPEND, "  Team X \"Fox\"\n\xE2\x98\x83  "));
  assert(!strcmp(np.ui.room_name_draft, "  Team X \"Fox\"  "));
  assert(netplay_room_name_input(NETPLAY_NAME_CONFIRM, NULL));
  assert(!np.ui.room_name_open && np.ui.cmd == NETPLAY_CMD_CREATE);
  assert(!strcmp(np.ui.cmd_text, "Team X \"Fox\""));
  np.ui.cmd = NETPLAY_CMD_NONE;
  assert(netplay_room_name_input(NETPLAY_NAME_OPEN, NULL));
  char long_name[120]; memset(long_name, 'A', sizeof long_name - 1); long_name[sizeof long_name - 1] = 0;
  netplay_room_name_input(NETPLAY_NAME_APPEND, long_name);
  assert(strlen(np.ui.room_name_draft) == 47);
  netplay_room_name_input(NETPLAY_NAME_BACKSPACE, NULL); assert(strlen(np.ui.room_name_draft) == 46);
  netplay_room_name_input(NETPLAY_NAME_CLEAR, NULL); assert(!np.ui.room_name_draft[0]);
  np.ui.cmd = NETPLAY_CMD_REFRESH;
  assert(!netplay_room_name_input(NETPLAY_NAME_CONFIRM, NULL)); assert(np.ui.room_name_open);
  netplay_room_name_input(NETPLAY_NAME_CANCEL, NULL); assert(!np.ui.room_name_open && np.ui.cmd == NETPLAY_CMD_REFRESH);
  np.ui.cmd = NETPLAY_CMD_NONE;
  netplay_room_name_input(NETPLAY_NAME_OPEN, NULL); netplay_room_name_input(NETPLAY_NAME_CONFIRM, NULL);
  assert(!strcmp(np.ui.cmd_text, "YAMPP room")); np.ui.cmd = NETPLAY_CMD_NONE;
  np.ui.room_id = 10; assert(!netplay_room_name_input(NETPLAY_NAME_OPEN, NULL)); np.ui.room_id = 0;
  const char* first = "1111111111111111111111111111111111111111111111111111111111111111";
  const char* second = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  NetplayMod mods[2] = {0};
  strcpy(mods[0].sha256, second); strcpy(mods[1].sha256, first);
  char csv[600], hashes[640], expected[640];
  assert(nm_hash_text(mods, 2, csv, sizeof csv));
  snprintf(expected, sizeof expected, "%s,%s", first, second); assert(!strcmp(csv, expected));
  nm_hash_json(csv, hashes, sizeof hashes);
  snprintf(expected, sizeof expected, "[\"%s\",\"%s\"]", first, second); assert(!strcmp(hashes, expected));
  strcpy(mods[1].sha256, mods[0].sha256); assert(!nm_hash_text(mods, 2, csv, sizeof csv));

  nm.pending_join = 123;
  char requirement[700];
  snprintf(requirement, sizeof requirement, "{\"op\":\"mods_required\",\"room\":123,\"mods\":[{\"sha256\":\"%s\",\"id\":\"example\",\"name\":\"Example\"}]}", first);
  nm_requirements(requirement);
  assert(np.ui.mod_prompt == 1 && np.ui.mod_pending_room == 123 && np.ui.mod_required_count == 1);
  assert(np.ui.room_id == 0 && !nm.process && !nm.activation);
  nm_command(NETPLAY_CMD_MOD_CANCEL, 0, NULL, NULL);
  assert(np.ui.room_id == 0 && !np.ui.mod_prompt && !nm.process && !nm.pending_join);

  nm.canceled = 0; nm.pending_join = 123;
  assert(!nm_room_ack("{\"id\":124}")); assert(nm.pending_join == 123);
  nm.pending_join = 0;
  char identity_json[1200];
  snprintf(identity_json,sizeof identity_json,"{\"fingerprint\":\"%s\",\"runtime\":\"%s\",\"game\":\"%s\"}",first,second,first);
  assert(!nm_read_identity(identity_json)); /* old helper is rejected */
  snprintf(identity_json,sizeof identity_json,"{\"runtimeIdentitySchema\":1,\"fingerprint\":\"%s\",\"runtime\":\"%s\",\"runtimeFileSha256\":\"%s\",\"game\":\"%s\",\"runtimeIdentity4x3\":\"%s\",\"runtimeIdentity16x9\":\"%s\",\"aspect4x3\":\"%s\",\"aspect16x9\":\"%s\"}",first,second,second,first,second,first,first,second);
  assert(nm_read_identity(identity_json)); assert(!strcmp(nm.runtime_file_sha256,second));
  char* schema=strstr(identity_json,"Schema\":1"); assert(schema);schema[8]='2';
  assert(!nm_read_identity(identity_json));
  nm.canceled = 0; nm.compat_ready = 1;
  strcpy(nm.aspect_fingerprint[0], first); strcpy(nm.aspect_fingerprint[1], second);
  strcpy(nm.runtime_identity[0], second); strcpy(nm.runtime_identity[1], first);
  assert(nm_bind_aspect() && nm_aspect_valid() && !strcmp(nm.fingerprint, first));
  nm_clear(0); assert(test_aspect_locked); /* leaving a room retains the lock */
  nm_release_aspect(); assert(!test_aspect_locked);
  aurora_link_widescreen(1,1); assert(nm_bind_aspect() && !strcmp(nm.fingerprint, second));
  assert(aurora_link_widescreen(1,0)==1 && nm_aspect_valid());
  test_aspect=0; assert(!nm_aspect_valid()); /* fail closed on a broken renderer */
  nm_release_aspect(); test_aspect_unavailable=1; assert(!nm_bind_aspect());
  assert(!test_aspect_locked && !nm.aspect_locked); test_aspect_unavailable=0;
  assert(nm_bind_aspect() && !strcmp(nm.fingerprint, first));
  nm.job=NM_COMPAT_HASH; nm.compat_ready=0; nm_fail("Injected verification failure");
  assert(!test_aspect_locked && !nm.aspect_locked && np.ui.phase==NETPLAY_PHASE_OFFLINE);
  nm.compat_ready=1; assert(nm_bind_aspect());
  strcpy(nm.loaded, first); strcpy(nm.room_hashes, first);
  char start[400]; snprintf(start, sizeof start, "{\"compatibility\":{\"fingerprint\":\"%s\"},\"required_mods\":[{\"sha256\":\"%s\"}]}", first, first);
  assert(nm_start_ok(start));
  test_costume_ready = 0; assert(!nm_start_ok(start));
  test_costume_ready = 1;
  snprintf(start, sizeof start, "{\"compatibility\":{\"fingerprint\":\"%s\"},\"required_mods\":[{\"sha256\":\"%s\"}]}", first, second);
  assert(!nm_start_ok(start));
  np.armed = 1; assert(!netplay_costume_activation_allowed());
  np.armed = 0; np.active = 1; assert(!netplay_costume_activation_allowed());
  np.active = 0; assert(netplay_costume_activation_allowed());

  assert(!nm_compatible("{\"compatibility\":null}"));
  snprintf(start, sizeof start, "{\"compatibility\":{\"fingerprint\":\"%s\"}}", second);
  assert(!nm_compatible(start));
  nm_welcome("{\"features\":[\"compat-v1\",\"mods-v1\"]}");
  assert(nm.has_compat && nm.has_protocol);
  nm_welcome("{\"features\":[],\"unrelated\":[\"compat-v1\",\"mods-v1\"]}");
  assert(!nm.has_compat && !nm.has_protocol);
  nm.queued_cmd = NETPLAY_CMD_CREATE; np.ui.phase = NETPLAY_PHASE_ROOM; np.ui.room_id = 12;
  nm_pending_command(); assert(nm.queued_cmd == 0 && !nm.process);
  np.ui.phase = NETPLAY_PHASE_OFFLINE; np.ui.room_id = 0;
  Context ctx = {0};
  np.active = 1; netplay_name_text(&ctx); assert(ctx.gpr[3] == 0);
  netplay_name_list_full(&ctx); assert(ctx.gpr[3] == 1);
  np.active = 0; netplay_name_text(&ctx); assert(ctx.gpr[3] == 0x12345678);
  netplay_name_list_full(&ctx); assert(ctx.gpr[3] == 0);
  ctx.ram = calloc(1, 0x2000000); assert(ctx.ram); ctx.ram_size = 0x2000000;
  ctx.gpr[3] = 0x80002000; memset(ctx.ram + 0x2000, 0x55, 0x100);
  netplay_clear_css_names(&ctx);
  for (unsigned offset = 0; offset < 0x100; ++offset) {
    int tag = offset >= 0x7A && (offset - 0x7A) % 0x24 == 0 && (offset - 0x7A) / 0x24 < 4;
    assert(ctx.ram[0x2000 + offset] == (tag ? 0x78 : 0x55));
  }
  unsigned char canonical[0x140], original[0x140];
  memset(ctx.ram + 0x4000, 0x17, 0x140); memcpy(original, ctx.ram + 0x4000, sizeof original);
  assert(netplay_prepare_vs(&ctx)); memcpy(canonical, ctx.ram + 0x4000, sizeof canonical);
  memset(ctx.ram + 0x4000, 0x35, 0x140); /* online selections change */
  netplay_restore_vs(&ctx); assert(!memcmp(ctx.ram + 0x4000, original, sizeof original));
  memset(ctx.ram + 0x4000, 0x58, 0x140); memcpy(original, ctx.ram + 0x4000, sizeof original);
  assert(netplay_prepare_vs(&ctx)); assert(!memcmp(ctx.ram + 0x4000, canonical, sizeof canonical));
  assert(original_vs_resets == 2);
  netplay_restore_vs(&ctx); assert(!memcmp(ctx.ram + 0x4000, original, sizeof original));
  free(ctx.ram);
  nm.compat_ready = 0;
#ifdef _WIN32
  nm.process = (HANDLE)1;
#else
  nm.process = 1;
#endif
   nm.job = NM_COMPAT_HASH;
  NetplayRules queued_rules; rules_default(&queued_rules);
  assert(nm_command(NETPLAY_CMD_CREATE, 0, "Queued host", &queued_rules));
  assert(nm.queued_cmd == NETPLAY_CMD_CREATE && !strcmp(nm.queued_text, "Queued host"));
  assert(!strcmp(np.ui.status, "Checking game files. Room request queued."));
  nm.process = 0; nm.job = NM_IDLE; nm.want_compat = nm.queued_cmd = 0; nm.compat_ready = 1;
  if (argc > 1 && !strcmp(argv[1], "--compatibility")) {
    nm_clear(1); nm.compat_ready = 0;
    nm_request_compatibility();
    DWORD started = GetTickCount();
    while ((nm.process || nm.want_compat) && GetTickCount() - started < 180000) { nm_poll(); Sleep(4); }
    assert(nm.compat_ready && nm.input_lock_count > 4 && !nm.process);
    const char* dol = getenv("MELEE_RUNTIME_DOL"); assert(dol);
#ifdef _WIN32
    HANDLE writer = CreateFileA(dol, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    assert(writer == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
    puts("Full real-game fingerprint verified; native handles blocked concurrent DOL writes.");
#else
    assert(nm_inputs_unchanged());
    puts("Full real-game fingerprint verified; advisory locks and input identity checks retained.");
#endif
    nm_unlock_inputs();
  } else if (argc > 2 && !strcmp(argv[1], "--room-version")) {
    assert(getenv("MELEE_WORKSHOP_TEST_MODS"));
    nm_clear(1); nm.canceled = 0; nm.pending_join = 123;
    snprintf(requirement, sizeof requirement, "{\"room\":123,\"mods\":[{\"sha256\":\"%s\",\"id\":\"room-link\"}]}", argv[2]);
    nm_requirements(requirement);
    assert(np.ui.mod_prompt == 1 && !nm.process && !nm.activation && np.ui.room_id == 0);
    assert(nm_command(NETPLAY_CMD_MOD_CONFIRM, 0, NULL, NULL));
    assert(nm.process && nm.job == NM_ROOM_INSTALL);
    DWORD started = GetTickCount();
    while (nm.process && GetTickCount() - started < 60000) { nm_poll(); Sleep(4); }
    assert(!nm.process && !nm.activation && nm.awaiting_room && !strcmp(nm.loaded, argv[2]));
    assert(np.ui.room_id == 0 && nm.pending_join == 123);
    nm_command(NETPLAY_CMD_MOD_CANCEL, 0, NULL, NULL);
    assert(!np.ui.room_id && !nm.pending_join && !nm.awaiting_room && !nm.loaded[0]);
    puts("Consented room-only version prepared; exact room handoff and cancellation preserved.");
  } else if (argc > 2 && !strcmp(argv[1], "--preview")) {
    nm_clear(1); np.ui.mod_catalog_count = 1;
    snprintf(np.ui.mod_catalog[0].sha256, sizeof np.ui.mod_catalog[0].sha256, "%s", argv[2]);
    assert(nm_command(NETPLAY_CMD_MOD_PREVIEW, 0, NULL, NULL));
    assert(np.ui.mod_inspect_loading && !np.ui.mod_job);
    assert(!strcmp(np.ui.mod_inspect_package, argv[2]));
    DWORD started = GetTickCount();
    while (nm.process && GetTickCount() - started < 60000) { nm_poll(); Sleep(4); }
    assert(!nm.process && !np.ui.mod_inspect_loading && np.ui.mod_inspect_ready == 1);
    assert(nm_hash_valid(np.ui.mod_inspect_sha256) && np.ui.mod_preview_path[0]);
    assert(!strcmp(np.ui.mod_preview_path + strlen(np.ui.mod_preview_path) - 4, ".png"));
    assert(np.ui.mod_catalog_count == 1 && !np.ui.mod_prompt && !nm.activation);
    /* A thumbnail error leaves the selected catalog and room state intact. */
    nm_preview_end(-1);
    assert(np.ui.mod_inspect_ready == -1 && !np.ui.mod_preview_path[0]);
    assert(np.ui.mod_catalog_count == 1 && !strcmp(np.ui.mod_inspect_package, argv[2]));
    puts("Native hidden worker loaded a hash-verified PNG only; preview failure preserved catalog state.");
  } else if (argc > 1 && !strcmp(argv[1], "--akaneia-local")) {
    assert(getenv("MELEE_CONTENT_RESTART"));
    nm_clear(1); nm.canceled=0; nm.pending_join=0;
    assert(nm_spawn(NM_UPSTREAM_DOWNLOAD, AKANEIA_SHA256, 0));
    DWORD started=GetTickCount();
    while(nm.process && GetTickCount()-started<180000) { nm_poll(); Sleep(4); }
    assert(!nm.process && netplay_content_reload_requested());
    assert(!strcmp(np.ui.mod_status,"Applying mods. Returning to the menu..."));
    puts("Verified local Akaneia archive accepted by the native worker; same-window reload requested.");
  } else if (argc > 1) {
    nm_clear(1);
#ifdef _WIN32
    SetEnvironmentVariableA("MELEE_MOD_REPOSITORY_URL", argv[1]);
#else
    setenv("MELEE_MOD_REPOSITORY_URL", argv[1], 1);
#endif
    assert(nm_spawn(NM_LIST, NULL, 0));
    DWORD started = GetTickCount();
    while (nm.process && GetTickCount() - started < 15000) { nm_poll(); Sleep(4); }
    assert(!nm.process && !np.ui.mod_catalog_loading && np.ui.mod_catalog_count == 0);
    assert(!strcmp(np.ui.mod_status, "No costume packs have been published yet."));
    puts("Native hidden worker completed a real localhost repository request.");
  }
  puts("Native mod prompt, canonical hash order, cancellation, and session guards passed.");
  return 0;
}
