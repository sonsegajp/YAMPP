#define main content_host_main
#include "../../hotload_main.c"
#undef main
#include <assert.h>
#include <shellapi.h>
static void write_plan(const char* path,const char* extra,int missing){
 FILE* f=fopen(path,"wb");assert(f);
 for(unsigned i=0;i<9;i++)if((int)i!=missing)fprintf(f,"%s\t%s\n",plan_keys[i],i==8?"0":i==0?"new-dol":"input");
 if(extra)fputs(extra,f);assert(!fclose(f));
}
int main(int argc,char**argv){
 assert(argc==2);const char* path=argv[1];
 env_set("MELEE_RUNTIME_DOL","previous-dol");
 const char* bad[]={"MELEE_RUNTIME_DOL\tduplicate\n","UNEXPECTED\tvalue\n","MELEE_INPUT\tnew\tfield\n","MELEE_INPUT\ttruncated"};
 for(unsigned i=0;i<4;i++){
  write_plan(path,bad[i],-1);assert(!plan_read(path));
  assert(!strcmp(getenv("MELEE_RUNTIME_DOL"),"previous-dol"));
 }
 for(int i=0;i<9;i++){write_plan(path,NULL,i);assert(!plan_read(path));assert(!strcmp(getenv("MELEE_RUNTIME_DOL"),"previous-dol"));}
 write_plan(path,NULL,-1);assert(plan_read(path));assert(!strcmp(getenv("MELEE_RUNTIME_DOL"),"new-dol"));
 const char* cases[]={"","plain","with spaces","C:\\Path with spaces\\","quotes\"inside","\\\"","a & b $(c) `d`"};
 for(unsigned i=0;i<sizeof cases/sizeof cases[0];i++){
  char quoted[1024],line[1050];assert(quote(quoted,sizeof quoted,cases[i]));snprintf(line,sizeof line,"tool %s",quoted);
  wchar_t wide[1050];assert(MultiByteToWideChar(CP_UTF8,0,line,-1,wide,1050));
  int n=0;wchar_t** parsed=CommandLineToArgvW(wide,&n);assert(parsed&&n==2);
  char actual[1024];assert(WideCharToMultiByte(CP_UTF8,0,parsed[1],-1,actual,sizeof actual,NULL,NULL));
  assert(!strcmp(actual,cases[i]));LocalFree(parsed);
 }
 char small[3];assert(!quote(small,sizeof small,"x"));
 puts("content host: transactional plans, required fields, duplicate rejection and Windows argument roundtrips passed");return 0;
}
