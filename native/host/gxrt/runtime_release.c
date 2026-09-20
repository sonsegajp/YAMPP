/* All guest/Online threads have joined before this function is called.
 * The renderer is owned by the persistent host and retains its window/device. */
#include "abi_recompcore.h"
extern void aurora_link_detach_runtime(void), music_release(void), mods_release(void);
extern void netplay_release(void), costumes_release(void), frontend_card_release(void);
extern void frontend_audio_release(void), frontend_bus_release(void), hle_thread_release(void);
extern void gx_translate_release(void), mex_runtime_shutdown(void), dvd_close_image(void), aram_free(void);
void runtime_release(Context* ctx){
 aurora_link_detach_runtime();
 music_release();mods_release();netplay_release();costumes_release();
 frontend_card_release();frontend_audio_release();hle_thread_release();
 gx_translate_release();frontend_bus_release();mex_runtime_shutdown();
 dvd_close_image();aram_free();cpu_free(ctx);
}
