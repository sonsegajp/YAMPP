/* Actual POSIX child lifecycle and advisory-lock behavior, using the production
 * worker. Build on Linux with the same includes/link flags as netplay_mods_test. */
#ifdef _WIN32
#error This regression exercises the POSIX implementation.
#endif
#define main common_tests
#include "netplay_mods_test.c"
#undef main

static void write_text(const char* path, const char* text) {
  FILE* file = fopen(path, "wb"); assert(file);
  assert(fwrite(text, 1, strlen(text), file) == strlen(text));
  assert(!fclose(file));
}
static void drain_worker(void) {
  DWORD start = GetTickCount();
  while (nm.process && GetTickCount() - start < 10000) { nm_poll(); Sleep(2); }
  assert(!nm.process);
}
int main(int argc, char** argv) {
  assert(common_tests(argc, argv) == 0);
  char temporary[] = "build/netplay-community/posix-worker-XXXXXX";
  mkdir("build/netplay-community", 0700); assert(mkdtemp(temporary));
  char folder[PATH_MAX], python[PATH_MAX], marker[PATH_MAX];
  assert(realpath(temporary, folder));
  /* Spaces, shell punctuation and UTF-8 must stay literal in argv. */
  assert(snprintf(python, sizeof python, "%s/python worker '$;\xE2\x98\x83", folder) < sizeof python);
  assert(snprintf(marker, sizeof marker, "%s/started", folder) < sizeof marker);
  write_text(python,
      "#!/usr/bin/python3\n"
      "import json,os,sys,time\n"
      "from pathlib import Path\n"
      "a=sys.argv[1:]\n"
      "assert Path(a[0]).name=='community.py'\n"
      "assert sys.stdin.read()==''\n"
      "mode=os.environ.get('NM_TEST_MODE','')\n"
      "Path(os.environ['NM_TEST_MARKER']).write_text(json.dumps(a))\n"
      "if mode=='sleep': time.sleep(60)\n"
      "if mode=='fail': sys.exit(7)\n"
      "assert a[1]=='list' and len(a)==4 and a[2]=='--output'\n"
      "Path(a[3]).write_text(json.dumps({'schema':1,'mods':[]}))\n");
  assert(!chmod(python, 0700));
  assert(!setenv("MELEE_WORKSHOP_PYTHON", python, 1));
  assert(!setenv("NM_TEST_MARKER", marker, 1));
  nm_clear(1); assert(nm_spawn(NM_LIST, NULL, 0));
  pid_t finished = nm.process; drain_worker();
  assert(!np.ui.mod_catalog_count && !np.ui.mod_catalog_loading && !np.ui.mod_job);
  assert(waitpid(finished, NULL, WNOHANG) < 0 && errno == ECHILD);
  assert(!strcmp(np.ui.mod_status, "No costume packs have been published yet."));
  unlink(marker); assert(!setenv("NM_TEST_MODE", "sleep", 1));
  assert(nm_spawn(NM_LIST, NULL, 0)); pid_t canceled = nm.process;
  DWORD start = GetTickCount();
  while (access(marker, F_OK) && GetTickCount() - start < 5000) Sleep(2);
  assert(!access(marker, F_OK));
  nm_command(NETPLAY_CMD_MOD_CANCEL, 0, NULL, NULL);
  assert(!nm.process && !nm.output[0] && !np.ui.mod_job);
  assert(waitpid(canceled, NULL, WNOHANG) < 0 && errno == ECHILD);
  assert(!setenv("NM_TEST_MODE", "fail", 1));
  assert(nm_spawn(NM_LIST, NULL, 0)); drain_worker();
  assert(!strcmp(np.ui.mod_status, "Could not verify the costume library. Check Workshop."));
  unsetenv("NM_TEST_MODE");

  char paths[5][PATH_MAX], doc[5 * PATH_MAX + 256];
  size_t used = (size_t)snprintf(doc, sizeof doc, "{\"files\":[");
  for (int i = 0; i < 5; ++i) {
    snprintf(paths[i], sizeof paths[i], "%s/input-%d", folder, i);
    write_text(paths[i], "original");
    used += (size_t)snprintf(doc + used, sizeof doc - used,
        "%s{\"path\":\"%s\"}", i ? "," : "", paths[i]);
  }
  snprintf(doc + used, sizeof doc - used, "]}");
  int writer = open(paths[0], O_WRONLY | O_CLOEXEC); assert(writer >= 0);
  assert(!flock(writer, LOCK_EX | LOCK_NB));
  assert(!nm_lock_inputs(doc)); nm_unlock_inputs(); close(writer);
  struct rlimit original_limit, limited;
  assert(!getrlimit(RLIMIT_NOFILE, &original_limit));
  limited = original_limit; limited.rlim_cur = 16; assert(!setrlimit(RLIMIT_NOFILE, &limited));
  assert(nm_lock_inputs(doc) && nm_inputs_unchanged());
  assert(!getrlimit(RLIMIT_NOFILE, &limited) && limited.rlim_cur == 37);
  assert(!setrlimit(RLIMIT_NOFILE, &original_limit));
  assert(nm.input_lock_count == 5);
  /* Demonstrate, rather than conceal, that a non-cooperating writer can bypass
   * flock. The next room/start preflight must invalidate compatibility. */
  write_text(paths[0], "changed outside advisory lock");
  assert(!nm_inputs_unchanged());
  nm.compat_ready = 1;
  assert(!nm_compatible("{}") && !nm.compat_ready);
  nm_unlock_inputs();
  assert(nm_lock_inputs(doc) && nm_inputs_unchanged());
  char replaced[PATH_MAX]; snprintf(replaced, sizeof replaced, "%s/replaced", folder);
  write_text(replaced, "changed outside advisory lock");
  assert(!rename(replaced, paths[0])); assert(!nm_inputs_unchanged());
  nm_unlock_inputs();
  assert(!unlink(paths[0])); assert(!symlink(paths[1], paths[0]));
  assert(!nm_lock_inputs(doc)); nm_unlock_inputs();
  for (int i = 0; i < 5; ++i) assert(!unlink(paths[i]));
  unlink(marker); assert(!unlink(python)); assert(!rmdir(folder));
  unsetenv("MELEE_WORKSHOP_PYTHON"); unsetenv("NM_TEST_MARKER");
  puts("POSIX argv/UTF-8, stdin, completion, cancellation/reaping, failure, advisory locks, mutation/replacement and symlink rejection passed.");
  return 0;
}
