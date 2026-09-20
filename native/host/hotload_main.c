/* Persistent application owner. The content core may be unloaded only after
 * its guest/network/decoder threads have joined and renderer callbacks detach. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static HMODULE renderer;
static void (*update)(void),(*end)(void);
static int (*begin)(void),(*quit)(void);
static void pump(void){if(update)update();if(begin&&begin()){if(end)end();}}
static void env_set(const char*key,const char*value){_putenv_s(key,value);SetEnvironmentVariableA(key,*value?value:NULL);}
static const char* plan_keys[]={
 "MELEE_RUNTIME_DOL","MELEE_FST","MELEE_DISC","MELEE_MEX_BASE_DOL",
 "MELEE_UNICORN_LIBRARY","MELEE_MOD_REGISTRY","MELEE_CONTENT_RETURN",
 "MELEE_CONTENT_JOIN_ROOM","MELEE_CONTENT_ENABLED","MELEE_NETPLAY_SERVER",
 "MELEE_NETPLAY_NAME","MELEE_INPUT","MELEE_TEST_MENU","MELEE_NETPLAY_AUTO",
 "MELEE_NETPLAY_INPUT","MELEE_CAPTURE_DIR"
};
#define PLAN_KEYS (sizeof plan_keys/sizeof plan_keys[0])
typedef struct { unsigned seen; char values[PLAN_KEYS][8192]; } ContentPlan;
/* Validate completely before changing any process environment. */
static int plan_read(const char* path){
 FILE* f=fopen(path,"rb");if(!f)return 0;
 ContentPlan* plan=calloc(1,sizeof *plan);if(!plan){fclose(f);return 0;}
 char line[8192];int ok=1;
 while(fgets(line,sizeof line,f)){
  char* tab=strchr(line,'\t');char* nl=strchr(line,'\n');
  if(!tab||!nl||tab>nl){ok=0;break;}
  *tab++=0;*nl=0;size_t n=strlen(tab);if(n&&tab[n-1]=='\r')tab[--n]=0;
  if(strchr(tab,'\t')||strchr(tab,'\r')){ok=0;break;}
  unsigned key=0;while(key<PLAN_KEYS&&strcmp(line,plan_keys[key]))key++;
  if(key==PLAN_KEYS||(plan->seen&(1u<<key))){ok=0;break;}
  plan->seen|=1u<<key;memcpy(plan->values[key],tab,n+1);
 }
 ok=ok&&!ferror(f)&&(plan->seen&511u)==511u;
 fclose(f);
 if(ok)ok=plan->values[0][0]&&plan->values[1][0]&&plan->values[2][0]&&plan->values[5][0]
   &&(!strcmp(plan->values[8],"0")||!strcmp(plan->values[8],"1"));
 if(ok)for(unsigned i=0;i<PLAN_KEYS;i++)if(plan->seen&(1u<<i))env_set(plan_keys[i],plan->values[i]);
 free(plan);return ok;
}
/* Quote argv for CreateProcess, without cmd.exe or shell interpolation. */
static int quote(char*out,size_t cap,const char*arg){size_t n=0;if(cap<3)return 0;out[n++]='"';unsigned slashes=0;for(;;arg++){if(*arg=='\\'){slashes++;continue;}unsigned copies=slashes*((*arg=='"'||!*arg)?2:1);while(copies--){if(n+2>=cap)return 0;out[n++]='\\';}slashes=0;if(!*arg)break;if(*arg=='"'){if(n+2>=cap)return 0;out[n++]='\\';}if(n+2>=cap)return 0;out[n++]=*arg;}out[n++]='"';out[n]=0;return(int)n;}
static int prepare(const char*path,int recover){const char*python=getenv("MELEE_WORKSHOP_PYTHON");const char*script=getenv("MELEE_CONTENT_PLAN_SCRIPT");if(!python||!script)return 0;char command[32768];size_t n=0;const char*args[]={python,script,"--output",path,recover?"--recover":"--resume"};for(unsigned i=0;i<5;i++){int z=quote(command+n,sizeof command-n,args[i]);if(!z)return 0;n+=z;if(i<4)command[n++]=' ';}command[n]=0;STARTUPINFOA si={.cb=sizeof si};PROCESS_INFORMATION pi={0};if(!CreateProcessA(NULL,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi))return 0;CloseHandle(pi.hThread);DWORD code=1,started=GetTickCount();while(WaitForSingleObject(pi.hProcess,15)==WAIT_TIMEOUT){pump();if((quit&&quit())||GetTickCount()-started>120000){TerminateProcess(pi.hProcess,1);break;}}WaitForSingleObject(pi.hProcess,1000);GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hProcess);return code==0&&plan_read(path);}
static LONG CALLBACK host_fault(EXCEPTION_POINTERS* ep){
 CONTEXT trace=*ep->ContextRecord;
 fprintf(stderr,"[content-host-fault] code=%lx\n",ep->ExceptionRecord->ExceptionCode);
 for(unsigned i=0;i<20&&trace.Rip;i++){
  DWORD64 base=0;PRUNTIME_FUNCTION fn=RtlLookupFunctionEntry(trace.Rip,&base,NULL);
  HMODULE module=NULL;char name[MAX_PATH]={0};
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)trace.Rip,&module);
  if(module)GetModuleFileNameA(module,name,sizeof name);
  fprintf(stderr,"[content-host-stack] %s+0x%llx\n",name,(unsigned long long)(trace.Rip-(uintptr_t)module));
  if(!fn){trace.Rip=*(DWORD64*)trace.Rsp;trace.Rsp+=8;}
  else{PVOID data;DWORD64 frame;RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,trace.Rip,fn,&trace,&data,&frame,NULL);}
 }
 fflush(NULL);TerminateProcess(GetCurrentProcess(),1);return EXCEPTION_CONTINUE_SEARCH;
}
int main(int argc,char**argv){
 SetUnhandledExceptionFilter(host_fault);
 setvbuf(stdout,NULL,_IONBF,0);
 char core[4096];GetModuleFileNameA(NULL,core,sizeof core);char*dot=strrchr(core,'.');if(!dot)return 2;strcpy(dot,".core.dll");
 /* Setup has no installed game or launch plan yet. Dispatch extraction to
  * the existing core entry point before loading the renderer or reading any
  * gameplay state. The core handles --extract before initializing the game. */
 if(argc>1&&!strcmp(argv[1],"--extract")){
  if(argc!=4){fprintf(stderr,"Usage: YAMPP --extract <disc image> <output folder>\n");return 2;}
  HMODULE runtime=LoadLibraryA(core);
  if(!runtime){fprintf(stderr,"[setup] Runtime load failed %lu. Extract the complete YAMPP ZIP.\n",GetLastError());return 2;}
  int(*extract)(int,char**)=(void*)GetProcAddress(runtime,"yampp_runtime_main");
  if(!extract){fprintf(stderr,"[setup] Runtime entry point missing\n");FreeLibrary(runtime);return 2;}
  int result=extract(argc,argv);
  FreeLibrary(runtime);
  return result;
 }
 const char*render_path=getenv("MELEE_AURORA_DLL"),*plan=getenv("MELEE_CONTENT_BOOT_PLAN");
 if(!render_path||!plan||!plan_read(plan)){fprintf(stderr,"[content-host] Missing verified launch plan\n");return 2;}
 env_set("MELEE_RUNTIME_HOSTED","1");env_set("MELEE_RUNTIME_CORE",core);
 renderer=LoadLibraryA(render_path);if(!renderer){fprintf(stderr,"[content-host] Renderer load failed %lu\n",GetLastError());return 2;}
 update=(void*)GetProcAddress(renderer,"aushim_update");begin=(void*)GetProcAddress(renderer,"aushim_begin_frame");end=(void*)GetProcAddress(renderer,"aushim_end_frame");quit=(void*)GetProcAddress(renderer,"aushim_quit_requested");
 int code=0;unsigned generation=0;
 for(;;){
  HMODULE runtime=LoadLibraryA(core);if(!runtime){fprintf(stderr,"[content-host] Runtime load failed %lu\n",GetLastError());code=2;break;}
  int(*run)(int,char**)=(void*)GetProcAddress(runtime,"yampp_runtime_main");if(!run){code=2;FreeLibrary(runtime);break;}
  char dol[4096];snprintf(dol,sizeof dol,"%s",getenv("MELEE_RUNTIME_DOL"));
  char*args[]={argv[0],dol,argc>2?argv[2]:"0",NULL};
  fprintf(stderr,"[content-host] generation=%u pid=%lu enabled=%s\n",++generation,GetCurrentProcessId(),getenv("MELEE_CONTENT_ENABLED"));
  code=run(3,args);
  uintptr_t(*window_id)(void)=(void*)GetProcAddress(renderer,"aushim_content_window_id");
  fprintf(stderr,"[content-host] window=%llu generation=%u\n",(unsigned long long)(window_id?window_id():0),generation);
  if(!FreeLibrary(runtime)){fprintf(stderr,"[content-host] Runtime unload failed\n");code=2;break;}
  fprintf(stderr,"[content-host] generation=%u unloaded result=%d\n",generation,code);
  if(code!=73)break;
  char serial[32];snprintf(serial,sizeof serial,"%u",generation+1);env_set("MELEE_CONTENT_GENERATION",serial);
  env_set("MELEE_CONTENT_ERROR","");
  if(!prepare(plan,0)){
   if(quit&&quit()){code=0;break;}
   fprintf(stderr,"[content-host] Preparation failed; restoring previous content\n");
   if(!prepare(plan,1)){code=2;break;}
   env_set("MELEE_CONTENT_ERROR","Could not apply mods. Previous setup restored.");
  }
 }
 /* The renderer's C++ dependencies are process-owned. Keep them resident until
  * OS teardown; the core's threads, files and guest allocations already closed. */
 void(*shutdown)(void)=(void*)GetProcAddress(renderer,"aushim_shutdown");if(shutdown)shutdown();
 fprintf(stderr,"[content-host] renderer shutdown complete\n");
 /* All application work is explicitly stopped and flushed above. Bypass
  * dependency DLL detach callbacks under the Windows loader lock, matching
  * the native test runner's shutdown policy. Reloads never take this path. */
 fflush(NULL);TerminateProcess(GetCurrentProcess(),code==73?2:code);return code;
}
