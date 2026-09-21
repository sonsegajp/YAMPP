/* Repository helpers run in a separate hidden process. This network thread
 * never waits for HTTPS, ZIP verification, or filesystem installation. */
#ifndef _WIN32
#include <spawn.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/wait.h>
extern char** environ;
typedef struct {
  int fd;
  char* path;
  struct stat snapshot;
} NmInputLock;
#else
typedef HANDLE NmInputLock;
#endif
extern void costumes_set_room_mods(const char* sorted_hashes);
extern int costumes_active_ready(void);

#include "upstream_build.h"
#define NM_MAX 8
#define NM_RUNTIME_IDENTITY_SCHEMA 1 /* YAMPP-runtime-camera-aspect-v1 domain */
enum { NM_IDLE, NM_LIST, NM_CREATE, NM_JOIN, NM_ROOM_INSTALL, NM_CATALOG_INSTALL, NM_TOGGLE, NM_COMPAT_PLAN, NM_COMPAT_HASH, NM_PREVIEW, NM_UPSTREAM_DOWNLOAD };
static struct {
#ifdef _WIN32
  HANDLE process;
  wchar_t output[MAX_PATH], plan[MAX_PATH];
#else
  pid_t process;
  char output[PATH_MAX], plan[PATH_MAX];
#endif
  DWORD started;
  NmInputLock* input_locks;
  int input_lock_count, compat_ready, want_compat, want_list, connect_pending, hello_sent, has_compat;
  char spawn_reason[160];   /* why the helper could not start, when it could not */
  char fingerprint[65], runtime_hash[65], game_hash[65], aspect_fingerprint[2][65];
  char runtime_file_sha256[65], runtime_identity[2][65];
  int aspect_locked, bound_aspect;
  unsigned serial;
  int job, has_protocol, has_upstream, pending_join, awaiting_room, canceled, activation;
  DWORD activation_started;
  int count, index;
  NetplayMod required[NM_MAX];
  char loaded[600], room_hashes[600], pending_name[48];
  NetplayRules pending_rules, queued_rules;
  int queued_cmd, queued_arg; char queued_text[48];
  char inspect_hash[65];
} nm;

static int nm_hash_valid(const char* text) {
  if (strlen(text) != 64) return 0;
  for (unsigned i = 0; i < 64; ++i)
    if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return 0;
  return 1;
}
static int nm_read_identity(const char* json) {
  json_str(json, "fingerprint", nm.fingerprint, sizeof nm.fingerprint);
  json_str(json, "runtimeFileSha256", nm.runtime_file_sha256, sizeof nm.runtime_file_sha256);
  json_str(json, "runtimeIdentity4x3", nm.runtime_identity[0], sizeof nm.runtime_identity[0]);
  json_str(json, "runtimeIdentity16x9", nm.runtime_identity[1], sizeof nm.runtime_identity[1]);
  json_str(json, "aspect4x3", nm.aspect_fingerprint[0], sizeof nm.aspect_fingerprint[0]);
  json_str(json, "aspect16x9", nm.aspect_fingerprint[1], sizeof nm.aspect_fingerprint[1]);
  json_str(json, "runtime", nm.runtime_hash, sizeof nm.runtime_hash);
  json_str(json, "game", nm.game_hash, sizeof nm.game_hash);
  return !(!nm_hash_valid(nm.fingerprint) || !nm_hash_valid(nm.runtime_hash) || !nm_hash_valid(nm.game_hash)
          || json_int(json, "runtimeIdentitySchema", 0) != NM_RUNTIME_IDENTITY_SCHEMA
          || strcmp(nm.runtime_hash, nm.runtime_file_sha256)
          || !nm_hash_valid(nm.runtime_identity[0]) || !nm_hash_valid(nm.runtime_identity[1])
          || !strcmp(nm.runtime_identity[0], nm.runtime_identity[1])
          || !nm_hash_valid(nm.aspect_fingerprint[0]) || !nm_hash_valid(nm.aspect_fingerprint[1])
          || !strcmp(nm.aspect_fingerprint[0], nm.aspect_fingerprint[1]));
}
static void nm_select_aspect(void) {
  snprintf(nm.runtime_hash, sizeof nm.runtime_hash, "%s", nm.runtime_identity[nm.bound_aspect]);
  snprintf(nm.fingerprint, sizeof nm.fingerprint, "%s", nm.aspect_fingerprint[nm.bound_aspect]);
  fprintf(stderr, "[netplay-mods] compatibility %s (%d locked inputs, aspect %s)\n",
          nm.fingerprint, nm.input_lock_count, nm.bound_aspect ? "16:9" : "4:3");
}
static int nm_bind_aspect(void) {
  if (!nm.aspect_locked) {
    int value = aurora_link_aspect_lock(1);
    if (value < 0 || value > 1) return 0;
    nm.bound_aspect = value; nm.aspect_locked = 1;
    if (nm.compat_ready) nm_select_aspect();
  }
  return 1;
}
static int nm_aspect_valid(void) {
  return nm.aspect_locked && aurora_link_aspect_lock(-1) == 1 &&
         aurora_link_widescreen(0, 0) == nm.bound_aspect;
}
static void nm_release_aspect(void) {
  if (nm.aspect_locked) aurora_link_aspect_lock(0);
  nm.aspect_locked = 0;
}
static void nm_fail(const char* text);
/* Report why the helper could not start, falling back to what the caller was
 * trying to do when there is nothing more specific to say. */
static void nm_fail_spawn(const char* fallback) {
  nm_fail(nm.spawn_reason[0] ? nm.spawn_reason : fallback);
  nm.spawn_reason[0] = 0;
}
static void nm_status(const char* text) {
  lock(); snprintf(np.ui.mod_status, sizeof np.ui.mod_status, "%s", text); unlock();
  fprintf(stderr, "[netplay-mods] %s\n", text);
}
static int nm_compare(const void* a, const void* b) {
  return strcmp(((const NetplayMod*)a)->sha256, ((const NetplayMod*)b)->sha256);
}
static int nm_parse(const char* object, const char* field, NetplayMod* mods, int limit, int* more) {
  const char* cursor = json_find(object, field), *value;
  int count = 0;
  if (more) *more = 0;
  if (!cursor) return 0;
  if (*skip_ws(cursor) != '[') return -1;
  while ((value = json_next(&cursor))) {
    if (count == limit) { if (more) *more = 1; else return -1; break; }
    NetplayMod* mod = &mods[count]; memset(mod, 0, sizeof *mod);
    json_str(value, "sha256", mod->sha256, sizeof mod->sha256);
    if (!nm_hash_valid(mod->sha256)) return -1;
    json_str(value, "id", mod->id, sizeof mod->id);
    json_str(value, "name", mod->name, sizeof mod->name);
    json_str(value, "version", mod->version, sizeof mod->version);
    json_str(value, "base", mod->base, sizeof mod->base);
    json_str(value, "description", mod->description, sizeof mod->description);
    mod->bytes = (int)json_int(value, "size", 0);
    mod->costumes = (int)json_int(value, "costumeCount", 0);
    mod->installed = (int)json_int(value, "installed", 0);
    mod->enabled = (int)json_int(value, "enabled", 0);
    ++count;
  }
  return count;
}
static int nm_hash_text(NetplayMod* mods, int count, char* csv, unsigned cap) {
  if (count < 0 || count > NM_MAX) return 0;
  qsort(mods, (size_t)count, sizeof *mods, nm_compare);
  unsigned used = 0; csv[0] = 0;
  for (int i = 0; i < count; ++i) {
    if (!nm_hash_valid(mods[i].sha256) || (i && !strcmp(mods[i - 1].sha256, mods[i].sha256))) return 0;
    int n = snprintf(csv + used, cap - used, "%s%s", i ? "," : "", mods[i].sha256);
    if (n < 0 || (unsigned)n >= cap - used) return 0;
    used += (unsigned)n;
  }
  return 1;
}
static void nm_hash_json(const char* csv, char* output, unsigned cap) {
  unsigned used = 0; output[used++] = '[';
  for (const char* hash = csv; *hash && used + 69 < cap; hash += 64) {
    if (hash != csv) { output[used++] = ','; if (*hash == ',') ++hash; }
    output[used++] = '"'; memcpy(output + used, hash, 64); used += 64; output[used++] = '"';
  }
  output[used++] = ']'; output[used] = 0;
}
static void nm_delete_plan(void) {
  if (!nm.plan[0]) return;
#ifdef _WIN32
  DeleteFileW(nm.plan);
#else
  unlink(nm.plan);
#endif
  nm.plan[0] = 0;
}
static void nm_unlock_inputs(void) {
  for (int i = 0; i < nm.input_lock_count; ++i) {
#ifdef _WIN32
    CloseHandle(nm.input_locks[i]);
#else
    close(nm.input_locks[i].fd);
    free(nm.input_locks[i].path);
#endif
  }
  free(nm.input_locks); nm.input_locks = NULL; nm.input_lock_count = 0;
  nm_delete_plan();
}
#ifndef _WIN32
static int nm_same_input(const struct stat* a, const struct stat* b) {
  return S_ISREG(b->st_mode) && a->st_dev == b->st_dev && a->st_ino == b->st_ino
      && a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec
      && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_sec == b->st_ctim.tv_sec
      && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}
#endif
static int nm_inputs_unchanged(void) {
#ifndef _WIN32
  /* flock is advisory: Unix editors can still change these files. Retain the
   * descriptors, compare both the open inode and current path before room
   * entry/Ready/start, and reject detected changes. This is not a mandatory
   * deny-write lock and cannot defeat a write racing after the last check. */
  for (int i = 0; i < nm.input_lock_count; ++i) {
    NmInputLock* input = &nm.input_locks[i]; struct stat held, current;
    if (fstat(input->fd, &held) || lstat(input->path, &current)
        || !nm_same_input(&input->snapshot, &held) || !nm_same_input(&input->snapshot, &current)) return 0;
  }
#endif
  return 1;
}
static const char* nm_lock_failure = "Close asset editors before verifying game data.";
static int nm_lock_inputs(const char* json) {
  nm_unlock_inputs();
  nm_lock_failure = "Close asset editors before verifying game data.";
  const char* cursor = json_find(json, "files"), *record;
  if (!cursor || *cursor != '[') return 0;
#ifndef _WIN32
  const char* scan = cursor; unsigned requested = 0;
  while (json_next(&scan)) if (++requested > 8192) return 0;
  struct rlimit limit;
  if (getrlimit(RLIMIT_NOFILE, &limit)) { nm_lock_failure = "Could not check game file access."; return 0; }
  rlim_t needed = (rlim_t)requested + 32;
  if (needed > limit.rlim_cur) {
    if (needed > limit.rlim_max) { nm_lock_failure = "Raise the open-file limit before connecting."; return 0; }
    limit.rlim_cur = needed;
    if (setrlimit(RLIMIT_NOFILE, &limit)) { nm_lock_failure = "Could not open enough game files for verification."; return 0; }
  }
#endif
  nm.input_locks = calloc(8192, sizeof *nm.input_locks);
  if (!nm.input_locks) return 0;
  while ((record = json_next(&cursor))) {
    if (nm.input_lock_count == 8192) return 0;
    char path[4096]; json_str(record, "path", path, sizeof path);
#ifdef _WIN32
    wchar_t wide[4096];
    if (!path[0] || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 4096)) return 0;
    HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    nm.input_locks[nm.input_lock_count++] = file;
#else
    if (!path[0]) return 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    NmInputLock* input = &nm.input_locks[nm.input_lock_count];
    if (flock(fd, LOCK_SH | LOCK_NB) || fstat(fd, &input->snapshot)
        || !S_ISREG(input->snapshot.st_mode) || !(input->path = strdup(path))) { close(fd); return 0; }
    input->fd = fd; ++nm.input_lock_count;
#endif
  }
  return nm.input_lock_count >= 5 && nm_inputs_unchanged();
}
static void nm_close_worker(int terminate) {
  if (nm.process) {
#ifdef _WIN32
    if (terminate) TerminateProcess(nm.process, 1);
    CloseHandle(nm.process); nm.process = NULL;
#else
    if (terminate) {
      /* Kill only our child; the platform TerminateProcess shim exits self. */
      kill(nm.process, SIGKILL);
      while (waitpid(nm.process, NULL, 0) < 0 && errno == EINTR) {}
    }
    nm.process = 0;
#endif
  }
  if (nm.output[0]) {
#ifdef _WIN32
    DeleteFileW(nm.output);
#else
    unlink(nm.output);
#endif
    nm.output[0] = 0;
  }
}
/* 1: running, 0: reaped/completed, -1: query failed. */
static int nm_worker_status(DWORD* code) {
#ifdef _WIN32
  if (!GetExitCodeProcess(nm.process, code)) return -1;
  return *code == STILL_ACTIVE;
#else
  int result; pid_t child;
  do { child = waitpid(nm.process, &result, WNOHANG); } while (child < 0 && errno == EINTR);
  if (!child) return 1;
  if (child < 0) { if (errno == ECHILD) nm.process = 0; return -1; }
  nm.process = 0;
  *code = WIFEXITED(result) ? (DWORD)WEXITSTATUS(result) : 1;
  return 0;
#endif
}
static FILE* nm_output_file(void) {
#ifdef _WIN32
  return _wfopen(nm.output, L"rb");
#else
  int fd = open(nm.output, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return NULL;
  FILE* file = fdopen(fd, "rb"); if (!file) close(fd); return file;
#endif
}
static int nm_save_plan(const char* json) {
#ifdef _WIN32
  (void)json;
  int n = _snwprintf(nm.plan, MAX_PATH, L"%ls.plan", nm.output);
  return n > 0 && n < MAX_PATH && CopyFileW(nm.output, nm.plan, FALSE);
#else
  int n = snprintf(nm.plan, sizeof nm.plan, "%s.plan", nm.output);
  if (n <= 0 || n >= (int)sizeof nm.plan) return 0;
  int fd = open(nm.plan, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return 0;
  size_t length = strlen(json), done = 0;
  while (done < length) {
    ssize_t written = write(fd, json + done, length - done);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) { close(fd); return 0; }
    done += (size_t)written;
  }
  return close(fd) == 0;
#endif
}
static void nm_preview_end(int ready) {
  lock(); np.ui.mod_inspect_loading = np.ui.mod_job = 0;
  np.ui.mod_inspect_ready = ready;
  if (ready != 1) { np.ui.mod_preview_path[0] = 0; np.ui.mod_inspect_sha256[0] = 0; }
  unlock(); nm.job = NM_IDLE;
}
static void nm_clear(int restore) {
  nm_close_worker(1);
  if (!nm.compat_ready) nm_unlock_inputs();
  nm.want_compat = nm.want_list = nm.queued_cmd = nm.connect_pending = 0;
  nm.job = NM_IDLE; nm.activation = 0; nm.pending_join = 0; nm.awaiting_room = 0;
  nm.count = nm.index = 0; nm.loaded[0] = nm.room_hashes[0] = 0;
  lock(); np.ui.mod_job = 0; np.ui.mod_catalog_loading = 0; np.ui.mod_prompt = 0;
  np.ui.mod_pending_room = 0; np.ui.mod_required_count = 0;
  np.ui.mod_inspect_loading = np.ui.mod_inspect_ready = 0; np.ui.mod_preview_path[0] = np.ui.mod_inspect_package[0] = 0; unlock();
  nm.inspect_hash[0] = 0;
  if (restore) costumes_set_room_mods(NULL);
}
static void nm_fail(const char* text) {
  if (nm.job == NM_COMPAT_PLAN || nm.job == NM_COMPAT_HASH) { nm_release_aspect(); lock(); np.ui.phase = NETPLAY_PHASE_OFFLINE; unlock(); }
  nm_clear(!np.active && !np.armed);
  nm_status(text); status(text);
}
#ifdef _WIN32
static int nm_python_path(wchar_t* python, unsigned capacity) {
  DWORD length = GetEnvironmentVariableW(L"MELEE_WORKSHOP_PYTHON", python, capacity);
  if (length) {
    if (length >= capacity) return 0;
    DWORD attributes = GetFileAttributesW(python);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
  }
  length = GetFullPathNameW(L"tools\\python\\python.exe", capacity, python, NULL);
  if (length && length < capacity) {
    DWORD attributes = GetFileAttributesW(python);
    if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return 1;
  }
  length = SearchPathW(NULL, L"python.exe", NULL, capacity, python, NULL);
  return length > 0 && length < capacity;
}
/* Why the worker could not be started, when it could not be.
 *
 * Every caller turns a failure here into its own message ("Could not open the
 * costume library"), which says what did not happen but not why, and the
 * commonest cause by far is an installation that is missing files -- a
 * download still extracting when the game was launched, or an incomplete
 * copy. That is entirely fixable by the player, but only if they are told. */
static void nm_spawn_missing(const wchar_t* what, const wchar_t* where) {
  fprintf(stderr, "[netplay-mods] cannot start the helper: %ls not found at %ls\n",
          what, where ? where : L"(unresolved)");
  /* Recorded rather than shown here: every caller reports its own failure
   * afterwards, and a generic "could not start" would otherwise bury the one
   * detail the player can act on. */
  snprintf(nm.spawn_reason, sizeof nm.spawn_reason,
           "Installation is incomplete. Extract the whole download again, then retry.");
}
static int nm_spawn(int job, const char* hash, int enabled) {
  wchar_t python[MAX_PATH], script[MAX_PATH], folder[MAX_PATH], log_path[MAX_PATH], command[2048];
  nm.spawn_reason[0] = 0;
  if (nm.process) return 0;
  if (!nm_python_path(python, MAX_PATH)) { nm_spawn_missing(L"the bundled Python runtime", L"tools\\python"); return 0; }
  const wchar_t* wanted = job == NM_COMPAT_PLAN || job == NM_COMPAT_HASH ? L"tools\\modkit\\compatibility.py" : L"tools\\modkit\\community.py";
  if (!GetFullPathNameW(wanted, MAX_PATH, script, NULL)) { nm_spawn_missing(wanted, NULL); return 0; }
  if (GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES) { nm_spawn_missing(wanted, script); return 0; }
  /* The helper imports project_config from the installation's scripts folder;
   * without it every job dies at import time with nothing the player can read. */
  { wchar_t support[MAX_PATH];
    if (GetFullPathNameW(L"scripts\\project_config.py", MAX_PATH, support, NULL)
        && GetFileAttributesW(support) == INVALID_FILE_ATTRIBUTES) {
      nm_spawn_missing(L"scripts\\project_config.py", support); return 0;
    } }
  if (!GetFullPathNameW(L"build\\netplay-community", MAX_PATH, folder, NULL)) return 0;
  CreateDirectoryW(folder, NULL);
  _snwprintf(nm.output, MAX_PATH, L"%ls\\worker-%lu-%u.json", folder, GetCurrentProcessId(), ++nm.serial);
  _snwprintf(log_path, MAX_PATH, L"%ls\\worker-%lu.log", folder, GetCurrentProcessId());
  const wchar_t* action = job == NM_LIST ? L"list" : job == NM_CREATE || job == NM_JOIN ? L"active"
                             : job == NM_UPSTREAM_DOWNLOAD ? L"upstream-download" : job == NM_TOGGLE ? L"toggle" : job == NM_PREVIEW ? L"preview" : job == NM_ROOM_INSTALL ? L"room-prepare" : L"install";
  wchar_t extra[768] = {0};
  if (hash) {
    if (!nm_hash_valid(hash)) return 0;
    wchar_t wide_hash[65]; for (unsigned i = 0; i <= 64; ++i) wide_hash[i] = (unsigned char)hash[i];
    _snwprintf(extra, 160, job == NM_TOGGLE ? L" --sha256 %ls --enabled %d" : job == NM_UPSTREAM_DOWNLOAD ? L" --sha256 %ls --confirm" : L" --sha256 %ls", wide_hash, enabled);
  }
  if (job == NM_UPSTREAM_DOWNLOAD && nm.pending_join > 0) wcscat(extra, L" --room-join");
  if (job == NM_ROOM_INSTALL) {
    char csv[600]; wchar_t wide_csv[600];
    if (!nm_hash_text(nm.required, nm.count, csv, sizeof csv)) return 0;
    for (unsigned i = 0; i <= strlen(csv); ++i) wide_csv[i] = (unsigned char)csv[i];
    _snwprintf(extra, 768, L" --hashes \"%ls\"", wide_csv);
  }
  int length;
  if (job == NM_COMPAT_PLAN || job == NM_COMPAT_HASH) {
    wchar_t runtime[MAX_PATH]; if (!GetModuleFileNameW(NULL, runtime, MAX_PATH)) return 0;
    if (job == NM_COMPAT_PLAN) _snwprintf(extra, 768, L"--runtime \"%ls\" --plan", runtime);
    else _snwprintf(extra, 768, L"--runtime \"%ls\" --input-plan \"%ls\"", runtime, nm.plan);
    length = _snwprintf(command, 2048, L"\"%ls\" \"%ls\" %ls --output \"%ls\"", python, script, extra, nm.output);
  } else length = _snwprintf(command, 2048, L"\"%ls\" \"%ls\" %ls%ls --output \"%ls\"", python, script, action, extra, nm.output);
  if (length < 0 || length >= 2048) return 0;
  SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
  HANDLE log = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (log == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    return 0;
  }
  STARTUPINFOW start = {0}; start.cb = sizeof start;
  start.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES; start.wShowWindow = SW_HIDE;
  start.hStdOutput = start.hStdError = log; start.hStdInput = input;
  PROCESS_INFORMATION process = {0};
  BOOL ok = CreateProcessW(python, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &start, &process);
  CloseHandle(log); CloseHandle(input);
  if (!ok) return 0;
  CloseHandle(process.hThread); nm.process = process.hProcess;
#else
static int nm_python_path(char* python, unsigned capacity) {
  const char* configured = getenv("MELEE_WORKSHOP_PYTHON");
  struct stat info;
  if (configured && *configured) {
    int n = snprintf(python, capacity, "%s", configured);
    return n > 0 && (unsigned)n < capacity && !stat(python, &info)
        && S_ISREG(info.st_mode) && !access(python, X_OK);
  }
  const char* bundled = "tools/python/bin/python3";
  if (!stat(bundled, &info) && S_ISREG(info.st_mode) && !access(bundled, X_OK)) {
    return realpath(bundled, python) != NULL;
  }
  return snprintf(python, capacity, "%s", "python3") < (int)capacity;
}
static int nm_spawn(int job, const char* hash, int enabled) {
  char python[PATH_MAX], script[PATH_MAX], folder[PATH_MAX], log_path[PATH_MAX], runtime[PATH_MAX];
  char csv[600], enabled_text[16];
  if (nm.process || !nm_python_path(python, sizeof python)
      || !realpath(job == NM_COMPAT_PLAN || job == NM_COMPAT_HASH
          ? "tools/modkit/compatibility.py" : "tools/modkit/community.py", script)) return 0;
  if ((mkdir("build", 0700) && errno != EEXIST)
      || (mkdir("build/netplay-community", 0700) && errno != EEXIST)
      || !realpath("build/netplay-community", folder)) return 0;
  int n = snprintf(nm.output, sizeof nm.output, "%s/worker-%ld-%u.json", folder, (long)getpid(), ++nm.serial);
  if (n < 0 || n >= (int)sizeof nm.output) return 0;
  n = snprintf(log_path, sizeof log_path, "%s/worker-%ld.log", folder, (long)getpid());
  if (n < 0 || n >= (int)sizeof log_path) return 0;
  char* args[16]; unsigned count = 0;
  args[count++] = python; args[count++] = script;
  if (job == NM_COMPAT_PLAN || job == NM_COMPAT_HASH) {
    ssize_t got = readlink("/proc/self/exe", runtime, sizeof runtime - 1);
    if (got <= 0 || got >= (ssize_t)sizeof runtime - 1) return 0;
    runtime[got] = 0; args[count++] = "--runtime"; args[count++] = runtime;
    if (job == NM_COMPAT_PLAN) args[count++] = "--plan";
    else { args[count++] = "--input-plan"; args[count++] = nm.plan; }
  } else {
    args[count++] = job == NM_LIST ? "list" : job == NM_CREATE || job == NM_JOIN ? "active"
        : job == NM_UPSTREAM_DOWNLOAD ? "upstream-download" : job == NM_TOGGLE ? "toggle" : job == NM_PREVIEW ? "preview"
        : job == NM_ROOM_INSTALL ? "room-prepare" : "install";
    if (job == NM_ROOM_INSTALL) {
      if (!nm_hash_text(nm.required, nm.count, csv, sizeof csv)) return 0;
      args[count++] = "--hashes"; args[count++] = csv;
    } else if (hash) {
      if (!nm_hash_valid(hash)) return 0;
      args[count++] = "--sha256"; args[count++] = (char*)hash;
      if (job == NM_UPSTREAM_DOWNLOAD) {
        args[count++] = "--confirm";
        if (nm.pending_join > 0) args[count++] = "--room-join";
      }
      if (job == NM_TOGGLE) {
        snprintf(enabled_text, sizeof enabled_text, "%d", enabled);
        args[count++] = "--enabled"; args[count++] = enabled_text;
      }
    }
  }
  args[count++] = "--output"; args[count++] = nm.output; args[count] = NULL;
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions)) return 0;
  int error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (!error) error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_path,
      O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
  if (!error) error = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
  pid_t child = 0;
  if (!error) error = posix_spawnp(&child, python, &actions, NULL, args, environ);
  posix_spawn_file_actions_destroy(&actions);
  if (error) return 0;
  nm.process = child;
#endif
  nm.started = GetTickCount(); nm.job = job;
  lock(); np.ui.mod_job = job == NM_PREVIEW ? 0 : job == NM_LIST ? 1 : 2; np.ui.mod_catalog_loading = job == NM_LIST;
  if (job == NM_LIST) np.ui.mod_progress = 0; unlock();
  return 1;
}
static void nm_request_compatibility(void) {
  if (nm.compat_ready) return;
  nm.want_compat = 1;
  if (!nm.process && !nm.activation) {
    if (!nm_spawn(NM_COMPAT_PLAN, NULL, 0)) nm_fail_spawn("Could not verify the native game build.");
    else { nm_status("Verifying native build and original game data..."); status("Checking game files..."); }
  }
}
static void nm_activate(int action, const char* csv) {
  snprintf(nm.loaded, sizeof nm.loaded, "%s", csv);
  nm.activation = action; nm.activation_started = GetTickCount();
  costumes_set_room_mods(nm.loaded);
  nm_status("Preparing matching costumes...");
}
static void nm_send_join(void) {
  char hashes[640], line[900]; nm_hash_json(nm.loaded, hashes, sizeof hashes);
  snprintf(line, sizeof line, "{\"op\":\"join\",\"room\":%d,\"installed_mods\":%s}", nm.pending_join, hashes);
  nm.awaiting_room = 1; tcp_send_line(line); nm_status("Joining room...");
}
static void nm_after_activation(void) {
  if (!nm_inputs_unchanged()) {
    nm.compat_ready = 0; nm_fail("Game files changed. Reconnect before joining a room."); return;
  }
  int action = nm.activation; nm.activation = 0;
  lock(); np.ui.mod_job = 0; np.ui.mod_progress = 100; unlock();
  if (action == NM_CREATE) {
    char hashes[640], line[1700], escaped[120], rules[240];
    nm_hash_json(nm.loaded, hashes, sizeof hashes);
    json_escape(nm.pending_name, escaped, sizeof escaped); rules_to_json(&nm.pending_rules, rules, sizeof rules);
    snprintf(line, sizeof line, "{\"op\":\"create\",\"name\":\"%s\",\"max\":2,\"rules\":%s,\"required_mods\":%s,\"installed_mods\":%s}", escaped, rules, hashes, hashes);
    nm.awaiting_room = 1; tcp_send_line(line); nm_status("Creating room...");
  } else if (action == NM_JOIN || action == NM_ROOM_INSTALL) nm_send_join();
}
static void nm_installed(void) {
  if (nm.job == NM_ROOM_INSTALL) {
    char csv[600];
    if (!nm_hash_text(nm.required, nm.count, csv, sizeof csv)) nm_fail("Room costume requirements are invalid.");
    else { nm.job = NM_IDLE; nm_activate(NM_ROOM_INSTALL, csv); }
  } else {
    nm.job = NM_IDLE; lock(); np.ui.mod_job = 0; np.ui.mod_prompt = 0; np.ui.mod_required_count = 0; unlock();
    costumes_set_room_mods(NULL);
    nm_status("Mod library updated.");
    if (!nm_spawn(NM_LIST, NULL, 0)) nm_status("Costumes saved. Refresh the library to continue.");
  }
}
static volatile LONG content_reload;
int netplay_content_reload_requested(void){return (int)InterlockedCompareExchange(&content_reload,0,0);}
static int nm_reload_content(void){
 const char* path=getenv("MELEE_CONTENT_RESTART");
 if(!path||!*path||np.ui.room_id||np.active||np.armed)return 0;
 char temp[4096],server[300],name[80];
 if(snprintf(temp,sizeof temp,"%s.tmp",path)>=(int)sizeof temp)return 0;
 json_escape(np.ui.server,server,sizeof server);json_escape(np.ui.name,name,sizeof name);
 FILE* f=fopen(temp,"wb");if(!f)return 0;
 int ok=fprintf(f,"{\"schema\":1,\"room\":%d,\"server\":\"%s\",\"name\":\"%s\"}\n",nm.pending_join,server,name)>0;
 if(fclose(f))ok=0;
#ifdef _WIN32
 if(ok)ok=MoveFileExA(temp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
 if(ok)ok=rename(temp,path)==0;
#endif
 if(!ok){remove(temp);return 0;}
 nm_status("Applying mods. Returning to the menu...");InterlockedExchange(&content_reload,1);return 1;
}
/* What the worker says it is doing, while it is still doing it.
 *
 * Preparing Akaneia takes minutes of disc patching, extraction and import. A
 * caption with a bar frozen at zero is indistinguishable from a hang, and a
 * player who cannot tell the two apart kills the job -- usually most of the
 * way through. The worker rewrites a small file beside its output as each
 * stage begins; this reads whichever version is there. A half-written or
 * missed update costs nothing because the next one replaces it. */
static void nm_read_progress(void) {
  wchar_t path[MAX_PATH + 16];
  if (!nm.output[0]) return;
  _snwprintf(path, MAX_PATH + 16, L"%ls.progress", nm.output);
  FILE* file = _wfopen(path, L"rb");
  if (!file) return;
  char text[512];
  size_t got = fread(text, 1, sizeof text - 1, file);
  fclose(file);
  text[got] = 0;
  if (!got) return;
  long percent = json_int(text, "percent", -1);
  char message[160]; json_str(text, "message", message, sizeof message);
  lock();
  if (percent >= 0 && percent <= 100) np.ui.mod_progress = (int)percent;
  if (message[0]) snprintf(np.ui.mod_status, sizeof np.ui.mod_status, "%s", message);
  unlock();
}
static void nm_clear_progress(void) {
  wchar_t path[MAX_PATH + 16];
  if (!nm.output[0]) return;
  _snwprintf(path, MAX_PATH + 16, L"%ls.progress", nm.output);
  DeleteFileW(path);
}
static void nm_poll(void) {
  if (nm.process) {
    DWORD code = 0;
    int worker_state = nm_worker_status(&code);
    if (worker_state < 0) {
      if (nm.job == NM_PREVIEW) { nm_close_worker(1); nm_preview_end(-1); }
      else nm_fail("Costume helper stopped unexpectedly.");
      return;
    }
    if (worker_state > 0) {
      /* Only the long content import reports stages; the rest finish before a
       * bar would mean anything. */
      if (nm.job == NM_UPSTREAM_DOWNLOAD) nm_read_progress();
      if (nm.job == NM_PREVIEW && GetTickCount() - nm.started > 50000) { nm_close_worker(1); nm_preview_end(-1); }
      else if (GetTickCount() - nm.started > (nm.job==NM_UPSTREAM_DOWNLOAD?900000u:180000u)) nm_fail(nm.job==NM_UPSTREAM_DOWNLOAD?"Akaneia preparation timed out. See the import log.":"Costume request timed out. Try again.");
      return;
    }
    FILE* file = nm_output_file(); char* json = NULL;
    if (file) {
      fseek(file, 0, SEEK_END); long size = ftell(file); rewind(file);
      if (size > 0 && size <= 2 * 1024 * 1024) {
        json = malloc((size_t)size + 1);
        if (json) { size_t got = fread(json, 1, (size_t)size, file); json[got] = 0; }
      }
      fclose(file);
    }
    if (nm.job == NM_COMPAT_PLAN && !code && json && !json_find(json, "error")) {
      if (!nm_lock_inputs(json)) { free(json); nm_fail(nm_lock_failure); return; }
      if (!nm_save_plan(json)) { free(json); nm_fail("Could not prepare game data verification."); return; }
    }
    nm_clear_progress();
    nm_close_worker(0);
    if (code || !json || json_find(json, "error")) {
      if (nm.job == NM_PREVIEW) { free(json); nm_preview_end(-1); return; }
      if (json) fprintf(stderr, "[netplay-mods] helper failed: %.1000s\n", json);
      const char* failure = nm.job == NM_COMPAT_PLAN || nm.job == NM_COMPAT_HASH
          ? "Could not verify the native build and original game data."
          : nm.job == NM_UPSTREAM_DOWNLOAD ? "Akaneia import failed. Check the archive and import log." : "Could not verify the costume library. Check Workshop.";
      if ((nm.job == NM_CREATE || nm.job == NM_JOIN) && json && strstr(json, "must be published or downloaded"))
        failure = "Publish or disable unverified costume packs.";
      if (nm.job == NM_UPSTREAM_DOWNLOAD && json) {
        char reason[32]; json_str(json,"uiCode",reason,sizeof reason);
        if (!strcmp(reason,"import_space")) failure="Akaneia needs 6 GB free on the YAMPP drive. Free space and retry.";
        else if (!strcmp(reason,"import_failed")) failure="Akaneia preparation failed. See build/akaneia-content import log.";
      }
      free(json); nm_fail(failure); return;
    }
    int job = nm.job;
    if (job == NM_COMPAT_PLAN) {
      nm.job = NM_IDLE;
      if (!nm_spawn(NM_COMPAT_HASH, NULL, 0)) { free(json); nm_fail_spawn("Could not verify original game data."); return; }
    } else if (job == NM_COMPAT_HASH) {
      if (!nm_read_identity(json)) {
        free(json); nm_fail("Update Online verification tools before connecting."); return;
      }
      if (!nm_inputs_unchanged()) {
        free(json); nm_fail("Native build verification did not complete."); return;
      }
      nm.compat_ready = 1; nm.want_compat = 0; nm.job = NM_IDLE;
      nm_delete_plan();
      lock(); np.ui.mod_job = 0; np.ui.mod_progress = 100; unlock();
      nm_status("Native build and game data verified.");
      if (nm.aspect_locked) nm_select_aspect();
    } else if (job == NM_UPSTREAM_DOWNLOAD) {
      char hash[65];json_str(json,"sha256",hash,sizeof hash);
      if (!json_int(json,"runtimeSupported",0) || strcmp(hash,AKANEIA_SHA256)) {
        free(json);nm_fail("Akaneia preparation verification failed.");return;
      }
      nm.job=NM_IDLE;
      if(!json_int(json,"restartRequired",0)||!nm_reload_content())nm_fail("Open the current YAMPP launcher to apply Akaneia.");
    } else if (job == NM_PREVIEW) {
      char package_hash[65], png_hash[65], path[1024];
      json_str(json, "sha256", package_hash, sizeof package_hash);
      json_str(json, "previewSha256", png_hash, sizeof png_hash);
      json_str(json, "previewPath", path, sizeof path);
      int available = (int)json_int(json, "previewAvailable", 0);
      int width = (int)json_int(json, "width", 0), height = (int)json_int(json, "height", 0);
      int bytes = (int)json_int(json, "previewSize", 0);
      size_t length = strlen(path);
      if (strcmp(package_hash, nm.inspect_hash) || (available && (!nm_hash_valid(png_hash)
          || length < 5 || strcmp(path + length - 4, ".png") || width < 1 || width > 4096
          || height < 1 || height > 4096 || bytes < 33 || bytes > 4 * 1024 * 1024))) {
        free(json); nm_preview_end(-1); return;
      }
      if (available) {
        lock(); snprintf(np.ui.mod_preview_path, sizeof np.ui.mod_preview_path, "%s", path);
        snprintf(np.ui.mod_inspect_sha256, sizeof np.ui.mod_inspect_sha256, "%s", png_hash); unlock();
      }
      nm_preview_end(available ? 1 : -1);
    } else if (job == NM_LIST) {
      NetplayMod* catalog = calloc(NETPLAY_MOD_CATALOG, sizeof *catalog); int more = 0;
      int count = catalog ? nm_parse(json, "mods", catalog, NETPLAY_MOD_CATALOG, &more) : -1;
      if (count < 0) { free(catalog); free(json); nm_fail("The costume catalog could not be read."); return; }
      lock(); memcpy(np.ui.mod_catalog, catalog, (size_t)count * sizeof *catalog);
      np.ui.mod_catalog_count = count; np.ui.mod_catalog_more = more;
      np.ui.mod_catalog_loading = np.ui.mod_job = 0; unlock();
      free(catalog); nm.job = NM_IDLE; nm_status(getenv("MELEE_CONTENT_ERROR") ? getenv("MELEE_CONTENT_ERROR") : count ? "Choose a mod." : "No mods available.");
    } else if (job == NM_CREATE || job == NM_JOIN) {
      NetplayMod active[NM_MAX]; char csv[600];
      int count = nm_parse(json, "mods", active, NM_MAX, NULL);
      if (count < 0 || !nm_hash_text(active, count, csv, sizeof csv)) {
        free(json); nm_fail("Installed costume verification failed."); return;
      }
      if (count && !nm.has_protocol) { free(json); nm_fail("This server does not support costume rooms yet."); return; }
      nm.job = NM_IDLE; nm_activate(job, csv);
    } else if (job == NM_ROOM_INSTALL) {
      NetplayMod prepared[NM_MAX]; char actual[600], expected[600];
      int count = nm_parse(json, "mods", prepared, NM_MAX, NULL);
      if (!json_int(json, "prepared", 0) || count != nm.count
          || !nm_hash_text(prepared, count, actual, sizeof actual)
          || !nm_hash_text(nm.required, nm.count, expected, sizeof expected) || strcmp(actual, expected)) {
        free(json); nm_fail("Room costume verification failed."); return;
      }
      nm_installed();
    } else if(job==NM_TOGGLE && json_int(json,"restartRequired",0)){
      nm.job=NM_IDLE;if(!nm_reload_content())nm_fail("Open the current YAMPP launcher to apply mods.");
    } else nm_installed();
    free(json);
  }
  if (nm.want_compat && !nm.process && !nm.activation && !nm.compat_ready) nm_request_compatibility();
  if (nm.want_list && !nm.process && !nm.activation && !nm.awaiting_room && !np.ui.room_id && !np.active && !np.armed) {
    nm.want_list = 0;
    if (!nm_spawn(NM_LIST, NULL, 0)) nm_fail_spawn("Could not open the costume library.");
  }
  if (nm.activation) {
    int ready = costumes_active_ready();
    if (ready > 0) nm_after_activation();
    else if (ready < 0 || GetTickCount() - nm.activation_started > 15000)
      nm_fail("Could not activate matching costumes. Room entry was canceled.");
  }
}
static void nm_welcome(const char* line) {
  const char* features = json_find(line, "features");
  const char* end = features && *features == '[' ? skip_value(features) : NULL;
  const char* mods = end ? strstr(features, "\"mods-v1\"") : NULL;
  const char* compat = end ? strstr(features, "\"compat-v1\"") : NULL;
  nm.has_protocol = mods && mods < end;
  nm.has_compat = compat && compat < end;
  const char* upstream=end?strstr(features,"\"upstream-builds-v1\""):NULL;nm.has_upstream=upstream&&upstream<end;
}
static int nm_compatible(const char* object) {
  if (!nm_inputs_unchanged()) { nm.compat_ready = 0; return 0; }
  if (!nm_aspect_valid()) return 0;
  const char* build = json_find(object, "upstream_build");
  if(g_mex_active){
    char id[40],version[40],sha[65];if(!build||*build!='{')return 0;
    json_str(build,"id",id,sizeof id);json_str(build,"version",version,sizeof version);json_str(build,"sha256",sha,sizeof sha);
    if(strcmp(id,"akaneia")||strcmp(version,AKANEIA_RELEASE)||strcmp(sha,AKANEIA_SHA256))return 0;
  }else if(build && strncmp(build,"null",4))return 0;
  const char* value = json_find(object, "compatibility"); char hash[65];
  if (!value || *value != '{' || !nm.compat_ready) return 0;
  json_str(value, "fingerprint", hash, sizeof hash);
  return !strcmp(hash, nm.fingerprint);
}

static void nm_upstream_required(const char* line) {
  int room=(int)json_int(line,"room",0);
  if(!nm.pending_join || room!=nm.pending_join || nm.canceled || nm.process || !nm.awaiting_room) return;
  const char* build=json_find(line,"build");
  if(build&&!strncmp(build,"null",4)&&g_mex_active){
    nm.awaiting_room=0;nm.count=0;nm.index=0;
    lock();np.ui.mod_prompt=4;np.ui.mod_pending_room=room;np.ui.mod_required_count=0;np.ui.mod_job=0;unlock();
    nm_status("This room uses original Melee. Disable Akaneia and join?");return;
  }
  char id[40],version[40],hash[65],source[32],repository[80];
  if(!build || *build!='{') {nm_fail("Invalid upstream room requirement.");return;}
  json_str(build,"id",id,sizeof id);json_str(build,"version",version,sizeof version);
  json_str(build,"sha256",hash,sizeof hash);json_str(build,"source",source,sizeof source);
  json_str(build,"repository",repository,sizeof repository);
  if(strcmp(id,"akaneia") || strcmp(version,AKANEIA_RELEASE) || strcmp(hash,AKANEIA_SHA256)
      || strcmp(source,"github") || strcmp(repository,"akaneia/akaneia-build")) {
    nm_fail("Update YAMPP for this upstream build version.");return;
  }
  memset(nm.required,0,sizeof nm.required);nm.count=1;nm.index=0;nm.awaiting_room=0;
  NetplayMod* mod=&nm.required[0];
  snprintf(mod->id,sizeof mod->id,"akaneia");snprintf(mod->name,sizeof mod->name,"The Akaneia Build");
  snprintf(mod->version,sizeof mod->version,"%s",AKANEIA_RELEASE);
  snprintf(mod->sha256,sizeof mod->sha256,"%s",AKANEIA_SHA256);mod->bytes=AKANEIA_DOWNLOAD_BYTES;
  lock();np.ui.mod_prompt=3;np.ui.mod_pending_room=room;np.ui.mod_required_count=1;
  np.ui.mod_required[0]=*mod;np.ui.mod_job=0;np.ui.mod_progress=0;unlock();
  nm_status("This room uses Akaneia. Download from official GitHub?");
}

static void nm_requirements(const char* line) {
  int room = (int)json_int(line, "room", 0);
  if (!nm.pending_join || room != nm.pending_join || nm.canceled) return;
  int count = nm_parse(line, "mods", nm.required, NM_MAX, NULL); char csv[600];
  if (count < 0 || !nm_hash_text(nm.required, count, csv, sizeof csv)) { nm_fail("Room costume requirements are invalid."); return; }
  nm.count = count; nm.index = 0; nm.awaiting_room = 0;
  lock(); np.ui.mod_prompt = 1; np.ui.mod_pending_room = room; np.ui.mod_required_count = count;
  np.ui.mod_job = 0; np.ui.mod_progress = 0;
  memcpy(np.ui.mod_required, nm.required, (size_t)count * sizeof nm.required[0]); unlock();
  nm_status(count ? "This room needs matching costumes. Download and join?" : "Use the original costumes for this room?");
}
static int nm_room_ack(const char* room) {
  if (nm.canceled) { tcp_send_line("{\"op\":\"leave\"}"); return 0; }
  if (nm.pending_join && (int)json_int(room, "id", 0) != nm.pending_join) return 0;
  NetplayMod required[NM_MAX]; char csv[600];
  int count = nm_parse(room, "required_mods", required, NM_MAX, NULL);
  if (count < 0 || !nm_compatible(room) || !nm_hash_text(required, count, csv, sizeof csv)
      || strcmp(csv, nm.loaded) || costumes_active_ready() <= 0) {
    tcp_send_line("{\"op\":\"leave\"}"); nm_fail("Match the host build, costumes and aspect ratio."); return 0;
  }
  snprintf(nm.room_hashes, sizeof nm.room_hashes, "%s", csv);
  nm.awaiting_room = 0; nm.pending_join = 0; nm.queued_cmd = 0;
  lock(); np.ui.mod_prompt = 0; np.ui.mod_pending_room = 0; np.ui.mod_job = 0;
  np.ui.mod_required_count = count; memcpy(np.ui.mod_required, required, (size_t)count * sizeof required[0]); unlock();
  return 1;
}
static int nm_start_ok(const char* line) {
  NetplayMod required[NM_MAX]; char csv[600];
  int count = nm_parse(line, "required_mods", required, NM_MAX, NULL);
  return !nm.process && !nm.activation && !nm.canceled && nm_compatible(line) && costumes_active_ready() > 0
      && nm_hash_text(required, count, csv, sizeof csv) && !strcmp(csv, nm.room_hashes) && !strcmp(csv, nm.loaded);
}
static int nm_command(int cmd, int arg, const char* text, const NetplayRules* rules) {
  if (cmd == NETPLAY_CMD_MOD_CANCEL) {
    int pending = nm.awaiting_room; nm.canceled = 1; nm_clear(!np.ui.room_id);
    if (pending) tcp_send_line("{\"op\":\"leave\"}");
    nm_status("Download canceled."); return 1;
  }
  /* Automatic thumbnails are expendable: they never delay explicit user work. */
  if (nm.job == NM_PREVIEW && cmd != NETPLAY_CMD_MOD_PREVIEW) {
    nm_close_worker(1); nm_preview_end(-1);
  }
  if (cmd == NETPLAY_CMD_MOD_INSPECT || cmd == NETPLAY_CMD_MOD_INSPECT_COSTUME) return 1;
  if (cmd == NETPLAY_CMD_MOD_PREVIEW) {
    if (np.ui.room_id || np.active || np.armed || np.ui.mod_prompt || np.ui.mod_catalog_loading
        || nm.activation || nm.awaiting_room || arg < 0 || arg >= np.ui.mod_catalog_count) return 1;
    if (nm.process && nm.job != NM_PREVIEW) return 1;
    char hash[65]; lock(); snprintf(hash, sizeof hash, "%s", np.ui.mod_catalog[arg].sha256); unlock();
    if (!nm_hash_valid(hash)) return 1;
    if (nm.job == NM_PREVIEW && !strcmp(hash, nm.inspect_hash)) return 1;
    if (nm.process) { nm_close_worker(1); nm_preview_end(-1); }
    snprintf(nm.inspect_hash, sizeof nm.inspect_hash, "%s", hash);
    lock(); snprintf(np.ui.mod_inspect_package, sizeof np.ui.mod_inspect_package, "%s", hash);
    np.ui.mod_inspect_loading = 1; np.ui.mod_inspect_ready = 0;
    np.ui.mod_preview_path[0] = np.ui.mod_inspect_sha256[0] = 0; unlock();
    if (!nm_spawn(NM_PREVIEW, hash, 0)) nm_preview_end(-1);
    return 1;
  }
  if (cmd == NETPLAY_CMD_MOD_REFRESH) {
    lock(); np.ui.mod_inspect_loading = np.ui.mod_inspect_ready = 0;
    np.ui.mod_preview_path[0] = np.ui.mod_inspect_package[0] = np.ui.mod_inspect_sha256[0] = 0; unlock();
    if (!nm.activation && !nm.awaiting_room && !np.ui.room_id && !np.active && !np.armed) {
      if (nm.process) {
        if (nm.job == NM_COMPAT_PLAN || nm.job == NM_COMPAT_HASH) {
          nm.want_list = 1; lock(); np.ui.mod_catalog_loading = 1; unlock();
        }
      } else if (!nm_spawn(NM_LIST, NULL, 0)) nm_fail_spawn("Could not open the costume library.");
    }
    return 1;
  }
  if (cmd == NETPLAY_CMD_MOD_PREPARE || cmd == NETPLAY_CMD_MOD_TOGGLE || cmd == NETPLAY_CMD_MOD_CONFIRM) {
    if (np.ui.room_id || np.active || np.armed) { nm_status("Leave the room before changing costumes."); return 1; }
    if (nm.process || nm.activation || nm.awaiting_room) return 1;
    if (cmd == NETPLAY_CMD_MOD_CONFIRM) {
      int prompt = np.ui.mod_prompt;
      if(prompt==4){nm.canceled=0;if(!nm_spawn(NM_TOGGLE,AKANEIA_SHA256,0))nm_fail_spawn("Could not switch to original Melee.");return 1;}
      if (prompt == 3) {
        nm.canceled=0;
        if(!nm_spawn(NM_UPSTREAM_DOWNLOAD,AKANEIA_SHA256,0)) nm_fail_spawn("Could not start the Akaneia importer.");
        return 1;
      }
      if (prompt != 1 && prompt != 2) return 1;
      nm.canceled = 0; nm.index = 0;
      if (prompt == 1 && !nm.count) nm_activate(NM_ROOM_INSTALL, "");
      else if (!nm_spawn(prompt == 1 ? NM_ROOM_INSTALL : NM_CATALOG_INSTALL, prompt == 1 ? NULL : nm.required[0].sha256, 0)) nm_fail_spawn("Could not start the costume download.");
      return 1;
    }
    if (arg < 0 || arg >= np.ui.mod_catalog_count) return 1;
    NetplayMod mod; lock(); mod = np.ui.mod_catalog[arg]; unlock();
    if (cmd == NETPLAY_CMD_MOD_TOGGLE) {
      if (!mod.installed) return 1;
      if (!nm_spawn(NM_TOGGLE, mod.sha256, !mod.enabled)) nm_fail_spawn("Could not update the costume setting.");
    } else {
      nm.count = 1; nm.required[0] = mod; nm.pending_join = 0;
      lock(); np.ui.mod_prompt = !strcmp(mod.id,"akaneia") ? 3 : 2; np.ui.mod_pending_room = 0; np.ui.mod_required_count = 1; np.ui.mod_required[0] = mod; unlock();
      nm_status(!strcmp(mod.id,"akaneia")?"Install Akaneia from the archive in user/imports?":"Download this costume pack?");
    }
    return 1;
  }
  if (cmd == NETPLAY_CMD_CREATE || cmd == NETPLAY_CMD_JOIN) {
    if (nm.compat_ready && !nm_inputs_unchanged()) { nm.compat_ready = 0; nm_unlock_inputs(); }
    if (np.ui.room_id || nm.job == NM_CREATE || nm.job == NM_JOIN) return 1;
    if (nm.activation || nm.awaiting_room || np.ui.mod_prompt || np.active || np.armed) return 1;
    if (!nm.compat_ready || np.ui.phase < NETPLAY_PHASE_LOBBY || nm.process) {
      nm.queued_cmd = cmd; nm.queued_arg = arg;
      snprintf(nm.queued_text, sizeof nm.queued_text, "%s", text ? text : "");
      if (rules) nm.queued_rules = *rules; else rules_default(&nm.queued_rules);
      if (!nm.compat_ready) nm_request_compatibility();
      status(!nm.compat_ready ? "Checking game files. Room request queued." : "Connecting. Room request queued.");
      return 1;
    }
    if (!nm.compat_ready || !nm.has_compat) { status("The server needs the current netplay update."); return 1; }
    nm.canceled = 0;
    if (cmd == NETPLAY_CMD_CREATE) {
      snprintf(nm.pending_name, sizeof nm.pending_name, "%s", text && *text ? text : "Melee room");
      nm.pending_rules = *rules; rules_clamp(&nm.pending_rules);
    } else nm.pending_join = arg;
    if (!nm_spawn(cmd == NETPLAY_CMD_CREATE ? NM_CREATE : NM_JOIN, NULL, 0)) nm_fail_spawn("Could not verify installed costumes.");
    return 1;
  }
  if ((cmd == NETPLAY_CMD_READY || cmd == NETPLAY_CMD_START) && !nm_inputs_unchanged()) {
    nm.compat_ready = 0; nm_fail("Game files changed. Reconnect before Ready."); return 1;
  }
  if ((cmd == NETPLAY_CMD_READY || cmd == NETPLAY_CMD_START) && (nm.process || nm.activation || costumes_active_ready() <= 0)) {
    status("Wait for costume verification before Ready."); return 1;
  }
  return 0;
}

static void nm_pending_command(void) {
  if (np.ui.room_id || np.ui.phase >= NETPLAY_PHASE_ROOM) { nm.queued_cmd = 0; return; }
  if (!nm.queued_cmd || !nm.compat_ready || nm.process || nm.activation || nm.awaiting_room
      || np.ui.phase < NETPLAY_PHASE_LOBBY || np.ui.mod_prompt || np.active || np.armed) return;
  int cmd = nm.queued_cmd, arg = nm.queued_arg; nm.queued_cmd = 0;
  nm_command(cmd, arg, nm.queued_text, &nm.queued_rules);
}
