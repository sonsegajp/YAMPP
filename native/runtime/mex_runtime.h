#ifndef YAMPP_MEX_RUNTIME_H
#define YAMPP_MEX_RUNTIME_H
/* Dynamic guest code only; stock builds never open the execution library. */
extern int g_mex_active;
int mex_runtime_init(Context* ctx);
int mex_runtime_entry(Context* ctx, uint32_t address, uint32_t size);
RecFn mex_runtime_lookup(uint32_t address);
#endif
