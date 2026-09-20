/* Callable synchronized simulation boundary based on the authoritative original
 * gm_801A4D34 (upstream/melee/src/melee/gm/gm_1A45.c). Native call stacks are
 * never copied or longjmp'ed: each replay invokes a complete returned step. */
extern size_t frontend_audio_snapshot(void*, int);
extern size_t frontend_io_snapshot(void*, int);
extern size_t frontend_bus_snapshot(void*, int);
extern size_t aram_snapshot(void*, int);
extern size_t hooks_rollback_snapshot(void*, int);
static size_t rollback_host_state(void* data, int restore) {
  unsigned char* bytes = data; size_t n = 0;
  n += frontend_audio_snapshot(bytes ? bytes + n : NULL, restore);
  n += frontend_io_snapshot(bytes ? bytes + n : NULL, restore);
  n += frontend_bus_snapshot(bytes ? bytes + n : NULL, restore);
  n += aram_snapshot(bytes ? bytes + n : NULL, restore);
  n += hooks_rollback_snapshot(bytes ? bytes + n : NULL, restore);
  return n;
}
#define RB_SCENE 0x80479D58u
#define RB_PAUSE (RB_SCENE + 0x10u)

static uint64_t rollback_guest64(Context* ctx, uint32_t fn, uint32_t arg) {
  Context saved = *ctx; ctx->gpr[3] = arg; lookup_function(fn)(ctx);
  uint64_t result = ((uint64_t)ctx->gpr[3] << 32) | ctx->gpr[4];
  uint64_t tb = ctx->timebase; *ctx = saved; ctx->timebase = tb; return result;
}
static void rollback_diagnose(Context* ctx, const char* phase, uint32_t frame) {
  const char* diagnostic=getenv("MELEE_RB_DIAG");
  if(!diagnostic)return;
  unsigned first=0,last=1;
  if(sscanf(diagnostic,"%u:%u",&first,&last)!=2){first=0;last=1;}
  if(frame<first||frame>last)return;
  int previous_service = rollback_service; rollback_service = 1;
  uint32_t seed = mem_read32(ctx, RNG_SEED_PTR);
  fprintf(stderr, "[rbdiag] %s frame=%u seedptr=%08X seed=%08X tb=%llu\n", phase, frame, seed,
          valid_guest(seed, 4) ? mem_read32(ctx, seed) : 0, (unsigned long long)ctx->timebase);
  static const uint16_t fields[] = { 0x10, 0x14, 0x2C, 0x80, 0x84, 0x88, 0x8C,
      0xB0, 0xB4, 0xE0, 0xEC, 0x620, 0x624, 0x65C, 0x668, 0x894, 0x89C, 0x1830, 0x1910 };
  for (unsigned p = 0; p < 4; ++p) {
    uint32_t gobj = call_guest(ctx, FN_PLAYER_ENTITY, p);
    if (!valid_guest(gobj, 0x30)) continue;
    uint32_t fighter = mem_read32(ctx, gobj + 0x2C);
    if (!valid_guest(fighter, 0x2400)) continue;
    fprintf(stderr, "[rbdiag] %s frame=%u P%u gobj=%08X fp=%08X", phase, frame, p, gobj, fighter);
    for (unsigned i = 0; i < sizeof fields / sizeof fields[0]; ++i)
      fprintf(stderr, " %03X=%08X", fields[i], mem_read32(ctx, fighter + fields[i]));
    fprintf(stderr, "\n");
  }
  rollback_service = previous_service;
}
/* Off-screen damage reads a flag normally produced by ifMagnify's draw
 * callback. Replay skips GPU work, so deriving that flag from a previously
 * drawn frame delays damage by the number of replayed frames. Recompute the
 * stock eligibility and stock camera projection before each simulation step.
 * References: ftLib_80086A8C, ifMagnify_802FBBDC, Fighter_8006ABEC. */
static void rollback_visibility(Context* ctx) {
  if(!rollback_match)return;
  int prior_service=rollback_service;rollback_service=1;
  uint32_t hud=call_guest(ctx,0x8016AE38u,0);
  int show=valid_guest(hud,6)&&mem_read8(ctx,hud+5)
      && !call_guest(ctx,0x802F36B0u,0) && !call_guest(ctx,0x80030130u,0);
  uint32_t camera=call_guest(ctx,0x80030A50u,0);
  for(unsigned port=0;port<6;port++) {
    uint32_t state=0x804A1E00u+port*0x10u;
    uint8_t flags=mem_read8(ctx,state);int offscreen=0;
    uint32_t gobj=call_guest(ctx,FN_PLAYER_ENTITY,port);
    if(valid_guest(camera,0x30)&&valid_guest(gobj,0x30)) {
      uint32_t fp=mem_read32(ctx,gobj+0x2C);
      if(valid_guest(fp,0x2400)) {
        uint8_t draw=mem_read8(ctx,fp+0x221F);
        if(!(draw&0x10)) {
          int outside=0;
          uint32_t subject=mem_read32(ctx,fp+0x890);
          if(!(mem_read8(ctx,fp+0x2229)&0x10) && !(mem_read8(ctx,fp+0x2220)&1)
              && valid_guest(subject,0x40)) {
            Context saved=*ctx;ctx->gpr[3]=subject;ctx->gpr[4]=fp+0x2188;
            lookup_function(0x80030CD8u)(ctx);outside=!ctx->gpr[3];
            uint64_t tb=ctx->timebase;*ctx=saved;ctx->timebase=tb;
          }
          draw=(draw&0x7F)|(outside?0x80:0);mem_write8(ctx,fp+0x221F,draw);
        }
        offscreen=show && !(flags&0x40) && (draw&0x80)
            && call_guest(ctx,0x80086ED0u,gobj);
      }
    }
    mem_write8(ctx,state,(flags&0x7F)|(offscreen?0x80:0));
  }
  rollback_service=prior_service;
}
static void rollback_simulate(Context* ctx, uint32_t on_frame, uint32_t frame) {
  rollback_step = 1; rollback_progress = 0; g_recomp_poll_budget = 4096;
  rollback_visibility(ctx);
  rollback_diagnose(ctx, "pre", frame);
  rollback_clock += 675000u;
  ctx->timebase = rollback_clock;
  /* Audio and deferred DMA/stream callbacks run at the same frame boundary in
   * live play and replay; asynchronous trace polling cannot change their order. */
  extern void arq_drain(Context*);
  extern void frontend_service_audio(Context*);
  rollback_service = 1;
  arq_drain(ctx); frontend_service_audio(ctx);
  rollback_service = 0;
  rollback_diagnose(ctx, "devices", frame);
  call_guest(ctx, 0x80019628u, 0);
  call_guest(ctx, 0x80392E80u, 0);
  mem_write32(ctx, 0x8046B0FCu, 0);
  netplay_master_status(ctx);
  if (mem_read32(ctx, 0x804D4A08u) >= 3) call_guest(ctx, 0x801A4970u, RB_PAUSE + 4);
  int run = (mem_read8(ctx, RB_PAUSE + 2) & 1) || !(mem_read8(ctx, RB_PAUSE) & 1);
  uint8_t flags = mem_read8(ctx, RB_SCENE + 0x38);
  mem_write8(ctx, RB_SCENE + 0x38, (uint8_t)((flags & 0x7F) | (run ? 0x80 : 0)));
  if (run) {
    call_guest(ctx, 0x80019900u, 0);
    if (call_guest(ctx, 0x80019A30u, 0)) call_guest(ctx, 0x801A3A74u, 0);
    if (call_guest(ctx, 0x80019A30u, 0) && on_frame) call_guest(ctx, on_frame, 0);
  }
  if (mem_read8(ctx, RB_PAUSE) != mem_read8(ctx, RB_PAUSE + 1) ||
      mem_read8(ctx, RB_PAUSE + 2) != mem_read8(ctx, RB_PAUSE + 3)) {
    uint64_t mask = rollback_guest64(ctx, 0x801A48A4u, mem_read8(ctx, RB_PAUSE));
    if (!(mem_read8(ctx, RB_SCENE + 0x38) & 0x80)) mask = UINT64_MAX;
    mem_write64(ctx, RB_SCENE + 0x20, mask);
    mem_write8(ctx, RB_PAUSE + 1, mem_read8(ctx, RB_PAUSE));
    mem_write8(ctx, RB_PAUSE + 3, mem_read8(ctx, RB_PAUSE + 2));
    mem_write8(ctx, RB_PAUSE + 2, 0);
  }
  uint64_t mask = mem_read64(ctx, RB_SCENE + 0x20);
  uint32_t mask_index = mem_read32(ctx, RB_SCENE + 0x34);
  uint64_t domain = mem_read64(ctx, 0x803DA8C8u + mask_index * 8);
  if (!call_guest(ctx, 0x80019A30u, 0)) mask |= domain;
  if (!call_guest(ctx, 0x80019A30u, 1)) mask |= ~domain;
  mem_write64(ctx, RB_SCENE + 0x28, mask);
  call_guest(ctx, 0x80027DF8u, 0);
  uint32_t before_procs = mem_read32(ctx, RB_SCENE + 0x30);
  if (before_procs) call_guest(ctx, before_procs, 0);
  rollback_diagnose(ctx, "before-procs", frame);
  call_guest(ctx, 0x80390CFCu, 0);
  rollback_diagnose(ctx, "post", frame);
  uint32_t count = mem_read32(ctx, RB_SCENE);
  if (count != UINT32_MAX - 1) mem_write32(ctx, RB_SCENE, count + 1);
  if ((mem_read8(ctx, RB_SCENE + 0x38) & 0x80) && call_guest(ctx, 0x80019A30u, 0)) {
    count = mem_read32(ctx, RB_SCENE + 8);
    if (count != UINT32_MAX - 1) mem_write32(ctx, RB_SCENE + 8, count + 1);
  }
  mem_write32(ctx, 0x8046B0FCu, 0);
  rollback_step = 0;
}

extern size_t frontend_thread_checkpoint(void*);
extern int frontend_thread_checkpoint_matches(const void*);
static void rollback_save(Context* ctx, uint32_t frame) {
  costume_art_frame_done(ctx); /* never snapshot transient presentation pixels */
  unsigned char* buffer = rb_save_buffer(&rollback, frame);
  memcpy(buffer, ctx, sizeof *ctx);
  memcpy(buffer + sizeof *ctx, ctx->ram, ctx->ram_size);
  size_t host_size = rollback_host_state(buffer + sizeof *ctx + ctx->ram_size, 0);
  size_t scheduler_size = frontend_thread_checkpoint(buffer + sizeof *ctx + ctx->ram_size + host_size);
  memcpy(buffer + sizeof *ctx + ctx->ram_size + host_size + scheduler_size, &rollback_clock, sizeof rollback_clock);
}
static int rollback_restore(Context* ctx, uint32_t frame) {
  costume_art_frame_done(ctx);
  const unsigned char* buffer = rb_load_buffer(&rollback, frame);
  if (!buffer) return 0;
  const unsigned char* scheduler = buffer + sizeof *ctx + ctx->ram_size + rollback_host_state(NULL, 0);
  /* Resuming or waking another thread can mutate native scheduler state even
   * without switching stacks. Require both metadata and continuations unchanged. */
  if (!frontend_thread_checkpoint_matches(scheduler)) {
    fprintf(stderr, "[netplay] rollback refused: native scheduler changed after snapshot\n");
    return 0;
  }
  /* GPU submission is external to simulation. Keep the current SDK GX cache,
   * VI/XFB ownership and draw-done queue coherent with the actual host GPU.
   * Match objects, RNG, pad history and the entire game heap are rewound. */
  unsigned char gx[0x4F4], vi[0x1F4], draw[0x18];
  memcpy(gx, ctx->ram + 0x4A76C8, sizeof gx);
  memcpy(vi, ctx->ram + 0x4C1D80, sizeof vi);
  memcpy(draw, ctx->ram + 0x4D7320, sizeof draw);
  memcpy(&rollback_clock, buffer + sizeof *ctx + ctx->ram_size + rollback_host_state(NULL, 0) + frontend_thread_checkpoint(NULL), sizeof rollback_clock);
  uint64_t now = rollback_clock;
  memcpy(ctx->ram, buffer + sizeof *ctx, ctx->ram_size);
  memcpy(ctx, buffer, sizeof *ctx); ctx->timebase = now;
  rollback_host_state((void*)(buffer + sizeof *ctx + ctx->ram_size), 1);
  memcpy(ctx->ram + 0x4A76C8, gx, sizeof gx);
  memcpy(ctx->ram + 0x4C1D80, vi, sizeof vi);
  memcpy(ctx->ram + 0x4D7320, draw, sizeof draw);
  return 1;
}
static void rollback_hashes(Context* ctx, uint32_t frame) {
  rollback_step = 1; rollback_service = 1;
  uint32_t hash = state_hash(ctx);
  rollback_service = 0; rollback_step = 0;
  lock(); np.hashes[frame & 255] = hash; unlock();
}
static void rollback_confirm_hashes(void) {
  lock();
  uint32_t end = rollback.confirmed < np.frame ? rollback.confirmed : np.frame;
  if (rollback.dirty != RB_NONE && end > rollback.dirty) end = rollback.dirty;
  for (uint32_t f = rollback_hash_sent; f < end; ++f)
    if (f % 60 == 0) fprintf(stderr, "[netplay] epoch %d frame %u hash %08X\n", np.epoch, f, np.hashes[f & 255]);
  rollback_hash_sent = end;
  for (int p = 0; p < 4; ++p) if (np.remote_hash_new[p]) {
    uint32_t f = np.remote_hash_frame[p];
    if (np.remote_hash_epoch[p] != (uint8_t)np.epoch || f == UINT32_MAX) { np.remote_hash_new[p] = 0; continue; }
    if (f >= end) continue;
    np.remote_hash_new[p] = 0;
    if (np.frame - f < 256 && np.hashes[f & 255] != np.remote_hash[p] && !np.desynced) {
      InterlockedExchange(&np.desynced, 1); np.ui.desynced = 1;
      fprintf(stderr, "[netplay] DESYNC at epoch %d frame %u: local %08X remote %08X\n", np.epoch, f, np.hashes[f & 255], np.remote_hash[p]);
    }
  }
  unlock();
}
static int rollback_correct(Context* ctx, uint32_t on_frame) {
  lock(); uint32_t first = rollback.dirty, end = np.frame;
  if (first == RB_NONE || first >= end) { unlock(); return 1; }
  rollback.dirty = RB_NONE; unlock();
  if (!rollback_restore(ctx, first)) { end_session("Rollback state unavailable or native scheduler changed"); return 0; }
  rollback_replay = 1;
  uint32_t replayed = 0;
  for (uint32_t f = first; f < end; ++f) {
    rollback_save(ctx, f);
    lock(); int ready = rb_inputs(&rollback, f, rollback_pads); unlock();
    if (!ready) { rollback_replay = 0; end_session("Rollback correction unavailable"); return 0; }
    rollback_simulate(ctx, on_frame, f); rollback_hashes(ctx, f);
    ++replayed;
    if (mem_read32(ctx, RB_SCENE + 0xC)) {
      lock(); np.frame = f + 1; np.ui.frame = (int)np.frame; rb_advance(&rollback, np.frame); unlock();
      break;
    }
  }
  rollback_replay = 0;
  ++rollback.rollback_count; rollback.replayed_frames += replayed;
  fprintf(stderr, "[netplay] rollback frame %u -> %u (%u replayed)\n", end, first, replayed);
  return 1;
}
static void rollback_render(Context* ctx) {
  call_guest(ctx, 0x800195D0u, 0);
  call_guest(ctx, 0x8033C898u, 0); /* GXInvalidateVtxCache */
  call_guest(ctx, 0x8033F270u, 0); /* GXInvalidateTexAll */
  call_guest(ctx, 0x80375538u, 0);
  call_guest(ctx, 0x80390FC0u, 0);
  call_guest(ctx, 0x803755A8u, 0);
  call_guest(ctx, 0x803761C0u, 0);
  uint32_t count = mem_read32(ctx, RB_SCENE + 4);
  if (count != UINT32_MAX - 1) mem_write32(ctx, RB_SCENE + 4, count + 1);
}
extern void netplay_return_to_lobby(void);
static void rollback_abort_scene(Context* ctx) {
  /* Use the game's major-mode cancellation path. A plain match-loop exit is
   * scored as a tied game and can launch local Sudden Death after disconnect. */
  call_guest(ctx, 0x801A42F8u, 1); /* gm_ChangeGameModeAfterCurrentScene(GM_MENU) */
  mem_write32(ctx, RB_SCENE + 0xC, 2);
  netplay_return_to_lobby();
  fprintf(stderr, "[netplay] canceled scene; returning to the netplay menu\n");
}
void netplay_scene_loop(Context* ctx) {
  if (!np.active || np.epoch == 0) { func_801A4D34(ctx); return; }
  if (!np.scene_epoch_ready) {
    end_session("Unexpected scene transition. Please start a new match.");
    rollback_abort_scene(ctx); call_guest(ctx,0x8037639Cu,0); return;
  }
  np.scene_epoch_ready=0;
  extern int mods_netplay_supported(void);
  if (rollback_match && !mods_netplay_supported()) {
    end_session("Rollback currently supports original fighters and stages only");
    rollback_abort_scene(ctx); call_guest(ctx, 0x8037639Cu, 0); return;
  }
  uint32_t on_frame = ctx->gpr[3];
  uint8_t ports = 0;
  for (unsigned p = 0; p < 4; ++p) if (np.port_ids[p]) ports |= (uint8_t)(1u << p);
  size_t state_size = rollback_match ? sizeof *ctx + ctx->ram_size + rollback_host_state(NULL, 0) + frontend_thread_checkpoint(NULL) + sizeof rollback_clock : 1;
  if (!rb_init(&rollback, ports, state_size)) {
    end_session("Not enough memory for rollback");
    rollback_abort_scene(ctx); call_guest(ctx, 0x8037639Cu, 0); return;
  }
  extern uint64_t frontend_guest_timebase(void);
  extern void frontend_audio_rebase(uint64_t);
  rollback_clock = frontend_guest_timebase();
  frontend_audio_rebase(rollback_clock);
  lock();
  rollback_running = 1; rollback_hash_sent = 0;
  for (unsigned p = 0; p < 4; ++p) if (ports & (1u << p)) {
    RbInput zero = {0};
    for (uint32_t f = 0; f < (uint32_t)np.delay; ++f) rb_receive(&rollback, p, f, &zero);
    for (unsigned i = 0; i < NP_RING; ++i) {
      const InputSlot* slot = &np.inputs[p][np.epoch & 1][i];
      if (slot->valid && slot->epoch == np.epoch && slot->frame >= (uint32_t)np.delay) {
        RbInput input; memcpy(&input, &slot->in, sizeof input); rb_receive(&rollback, p, slot->frame, &input);
      }
    }
  }
  unlock();
  if (rollback_match) fprintf(stderr, "[netplay] rollback enabled: window=%d, snapshot=%zu bytes\n", RB_WINDOW, rollback.state_size);
  else fprintf(stderr, "[netplay] synchronized scene driver: epoch=%d\n", np.epoch);
  Context caller = *ctx;
  /* Own the same 80-byte guest stack frame as the original scene driver, so
   * each callback has a private ABI parameter/linkage area during replay. */
  mem_write32(ctx, caller.gpr[1] - 80, caller.gpr[1]);
  ctx->gpr[1] -= 80;
  /* Match initialization and presentation remain the original guest routines. */
  call_guest(ctx, 0x801677C0u, RB_PAUSE);
  for (unsigned i = 0; i < 4; ++i) mem_write32(ctx, RB_SCENE + i * 4, 0);
  call_guest(ctx, 0x80376D04u, 2);
  call_guest(ctx, 0x8001CF18u, 0);
  rollback_diagnose(ctx, "entry", 0);
  extern volatile long g_frames_posted;
  long paced = g_frames_posted;
  DWORD exit_wait = 0, exit_sent = 0;
  while (np.active && !np.quit) {
    if (np.desynced) { end_session("Confirmed simulation mismatch; match stopped"); break; }
    while (g_frames_posted == paced && np.active && !np.quit) { recomp_poll(ctx); Sleep(1); }
    paced = g_frames_posted;
    if (!rollback_correct(ctx, on_frame)) break;
    rollback_confirm_hashes();
    /* Scene exits must be confirmed before assets and native callers unwind. */
    if (mem_read32(ctx, RB_SCENE + 0xC)) {
      lock(); int settled = rollback.confirmed >= np.frame && rollback.dirty == RB_NONE; unlock();
      if (settled) break;
      if (!exit_wait) exit_wait = GetTickCount();
      if (GetTickCount() - exit_sent >= 40) {
        send_inputs(np.frame + (uint32_t)np.delay - 1, NP_REDUNDANT);
        exit_sent = GetTickCount();
      }
      if (GetTickCount() - exit_wait > NP_STALL_TIMEOUT_MS) { end_session("Opponent did not confirm the match result"); break; }
      recomp_poll(ctx); continue;
    }
    exit_wait = 0;
    uint32_t f = np.frame;
    NetInput local = np.local_merge;
    /* Sample once per simulation frame, independent of wall-clock pad alarms. */
    extern int aurora_link_get_pad(unsigned, AushimPadStatus*);
    AushimPadStatus physical = {0};
    if (aurora_link_get_pad(0, &physical)) {
      local.buttons |= physical.buttons; local.sx = physical.stick_x; local.sy = physical.stick_y;
      local.cx = physical.cstick_x; local.cy = physical.cstick_y;
      local.lt = physical.trigger_left; local.rt = physical.trigger_right;
    }
    if (np.have_script) scripted_local(&local);
    memset(&np.local_merge, 0, sizeof np.local_merge); np.local_merge_valid = 0;
    lock();
    if (!get_input(np.local_port, np.epoch, f + (uint32_t)np.delay))
      store_input(np.local_port, np.epoch, f + (uint32_t)np.delay, &local);
    unlock();
    send_inputs(f + (uint32_t)np.delay, NP_REDUNDANT);
    DWORD started = GetTickCount();
    for (;;) {
      if (!rollback_correct(ctx, on_frame)) break;
      if (np.frame != f || mem_read32(ctx, RB_SCENE + 0xC)) break;
      lock(); int ready = (rollback_match || rollback.confirmed > f) && rb_inputs(&rollback, f, rollback_pads); unlock();
      if (ready || !np.active || np.quit) break;
      if (GetTickCount() - started > NP_STALL_TIMEOUT_MS) { end_session("Opponent stopped responding"); break; }
      lock(); np.ui.stalled_ms = (int)(GetTickCount() - started); unlock();
      recomp_poll(ctx); Sleep(1);
    }
    lock(); np.ui.stalled_ms = 0; unlock();
    if (!np.active || np.quit) break;
    if (np.frame != f || mem_read32(ctx, RB_SCENE + 0xC)) continue;
    if (rollback_match) rollback_save(ctx, f);
    rollback_simulate(ctx, on_frame, f); rollback_hashes(ctx, f);
    lock(); np.frame = f + 1; np.ui.frame = (int)np.frame; rb_advance(&rollback, np.frame); unlock();
    rollback_confirm_hashes();
    if (mem_read32(ctx, RB_SCENE + 0xC) != 2) rollback_render(ctx);
  }
  fprintf(stderr, "[netplay] rollback totals: corrections=%u replayed=%u\n", rollback.rollback_count, rollback.replayed_frames);
  costume_art_frame_done(ctx);
  lock(); rollback_running = 0; unlock(); rb_destroy(&rollback);
  /* Do not treat the switch back to the host clock as elapsed audio time. */
  frontend_audio_rebase(frontend_guest_timebase());
  if (!np.active) rollback_abort_scene(ctx);
  if (!mem_read32(ctx, RB_SCENE + 0xC)) mem_write32(ctx, RB_SCENE + 0xC, 2);
  call_guest(ctx, 0x8037639Cu, 0);
  uint64_t final_clock = ctx->timebase; *ctx = caller; ctx->timebase = final_clock;
}
