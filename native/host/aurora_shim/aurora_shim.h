#ifndef SA2B_RECOMP_AURORA_SHIM_H
#define SA2B_RECOMP_AURORA_SHIM_H

/* Stable, compiler-neutral DLL boundary between the MinGW runtime and the
 * MSVC-built Aurora/Dawn renderer. Keep this header C-only: no Aurora, SDL,
 * Dawn, or C++ types may cross the boundary. Calls are expected to be
 * serialized on the renderer's main thread unless documented otherwise. */

#include <stdint.h>

#if defined(_WIN32) && defined(AUSHIM_BUILD_DLL)
#define AUSHIM_API __declspec(dllexport)
#elif defined(_WIN32) && defined(AUSHIM_IMPORT_DLL)
#define AUSHIM_API __declspec(dllimport)
#else
#define AUSHIM_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AUSHIM_ABI_VERSION 1u

/* Aurora's public backend value. This is deliberately repeated here so a
 * MinGW consumer does not need any Aurora headers. */
enum {
  AUSHIM_BACKEND_VULKAN = 4,
};

enum {
  AUSHIM_GPU_INITIALIZED = 1u << 0,
  AUSHIM_GPU_FRAME_ACTIVE = 1u << 1,
  AUSHIM_GPU_VULKAN = 1u << 2,
  AUSHIM_GPU_CPU_ADAPTER = 1u << 3,
  AUSHIM_GPU_HARDWARE_ADAPTER = 1u << 4,
};

typedef struct AushimGpuInfo {
  uint32_t struct_size;
  uint32_t abi_version;
  int32_t backend;
  uint32_t flags;
  char adapter_name[256];
  char driver_description[256];
} AushimGpuInfo;

/* Compiler-neutral snapshot of the useful GameCube PAD fields. error uses the
 * SDK values: 0 connected, -1 no controller, -2 not ready, -3 transfer error. */
typedef struct AushimPadStatus {
  uint16_t buttons;
  int8_t stick_x;
  int8_t stick_y;
  int8_t cstick_x;
  int8_t cstick_y;
  uint8_t trigger_left;
  uint8_t trigger_right;
  uint8_t analog_a;
  uint8_t analog_b;
  int8_t error;
  uint8_t reserved;
  uint32_t extended_buttons;
} AushimPadStatus;

/* Lifetime and frame cadence. Both init functions return the selected Aurora
 * backend enum (AUSHIM_BACKEND_VULKAN, currently 4) on success and a negative
 * value on failure. This build accepts only backend=4 and allow_cpu=0.
 * update -> begin -> zero or more submissions -> end is the required per-frame
 * order. end and shutdown are idempotent at the ABI boundary. */
AUSHIM_API uint32_t aushim_get_abi_version(void);
AUSHIM_API int aushim_init(unsigned width, unsigned height);
AUSHIM_API int aushim_init_backend(unsigned width, unsigned height,
                                    int backend, int allow_cpu);
AUSHIM_API void aushim_update(void);
/* Pumps window/input events. Returns 0 while running, 1 after an exit event,
 * and -1 when the shim is not initialized. */
AUSHIM_API int aushim_update_status(void);
AUSHIM_API int aushim_quit_requested(void);
AUSHIM_API int aushim_begin_frame(void);
AUSHIM_API void aushim_end_frame(void);
AUSHIM_API void aushim_synchronize(void);
AUSHIM_API void aushim_shutdown(void);
AUSHIM_API int aushim_is_initialized(void);
AUSHIM_API int aushim_is_frame_active(void);

/* Copies the available prefix of AushimGpuInfo into out_info. Passing the
 * explicit size lets future ABI revisions append fields safely. */
AUSHIM_API int aushim_get_gpu_info(AushimGpuInfo* out_info,
                                   uint32_t out_info_size);

/* Returns 1 and fills a port snapshot for ports 0-3, or 0 for an invalid port,
 * null output, or uninitialized shim. A disconnected controller is a valid
 * snapshot with error=-1. PADRead is refreshed after every Aurora update.
 * Port 0 receives the recomp's deterministic keyboard map on every launch:
 * X/Z/C/V actions, Enter start, Q/E shoulders, Left Shift Z-trigger, WASD
 * main stick, arrows d-pad, and IJKL c-stick. This deliberately repairs stale
 * Aurora maps from other titles. Native gamepads remain active. */
AUSHIM_API int aushim_poll_pad(uint32_t port, AushimPadStatus* out_status);

/* Submit an already-translated, big-endian GX FIFO blob. The blob must not
 * contain guest pointers or nested CALL_DL commands; those are resolved by the
 * translation layer before crossing this DLL boundary. The bytes are copied by
 * Aurora before this call returns, so the caller may immediately reuse them. */
AUSHIM_API int aushim_submit_fifo(const void* data, uint32_t len);

/* Publishes all previously submitted FIFO bytes and blocks until Aurora's
 * FIFO worker has processed them.  Frame-scoped (returns 0 outside an active
 * frame).  Used as the machine-visible ordering fence for PE token/finish;
 * never performs presentation or readback. */
AUSHIM_API int aushim_fifo_drain(void);

/* Guest MEM1 remains owned by the recompilation runtime. These helpers support
 * Aurora-specific commands containing host pointers. */
AUSHIM_API void aushim_set_mem1(void* host_base, uint32_t size);
AUSHIM_API void* aushim_guest_to_host(uint32_t ea);

/* Compatibility exports retained for the existing GXRuntime frontend. */
AUSHIM_API int aushim_probe(void);
AUSHIM_API int aushim_call_dl(const void* data, uint32_t len);
AUSHIM_API int aushim_draw_sized(unsigned char draw_opcode,
                                 const void* verts, uint32_t len);
AUSHIM_API int aushim_draw_sized_guest(unsigned char draw_opcode,
                                       uint32_t guest_ea, uint32_t len);
AUSHIM_API int aushim_set_arraybase(uint32_t cp_idx, uint32_t guest_ea,
                                    uint32_t size, unsigned char le);
AUSHIM_API int aushim_set_arraystride(uint32_t cp_idx, uint32_t stride);
AUSHIM_API int aushim_call_dl_with_arrays(const uint32_t* eas,
                                          const uint32_t* sizes,
                                          const void* stream, uint32_t len);
AUSHIM_API int aushim_readback(uint8_t* dst, uint32_t* out_w,
                               uint32_t* out_h, uint32_t cap);

#if defined(AUSHIM_ENABLE_DIAGNOSTICS)
/* Bring-up helpers are intentionally absent from production builds because
 * they inject synthetic SDK GX state. */
AUSHIM_API int aushim_touch_gx(void);
AUSHIM_API int aushim_draw_test_triangle(void);
AUSHIM_API int aushim_setup_flat(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
