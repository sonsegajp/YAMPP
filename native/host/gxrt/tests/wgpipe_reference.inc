/* Pre-optimization FIFO transfer oracle. Host source only; no game data. */
static bool wr_wgpipe_reference(void* u, CPUState* c, u32 ea, u8 sz, u64 v)
{
    (void)u; (void)c; (void)ea;
    u8 buf[8];
    for (u8 i = 0; i < sz && i < 8; i++)          /* big-endian, MSB first */
        buf[i] = (u8)(v >> (8 * (sz - 1 - i)));
    /* Byte-level CP watch.
     *
     * The SDK writes WGPIPE with GX_WRITE_U8 -- one byte per store -- so a
     * 6-byte CP command never appears as a single multi-byte write. The
     * previous probe tested the first two bytes of a store and could not have
     * produced a hit; it reported zero and that zero meant nothing. This walks
     * bytes and can demonstrably fire (any CP write at all trips it). */
    {   static int st = 0; static u8 cp[6]; static long ncp = 0, nvcd = 0;
        for (u8 bi = 0; bi < sz && bi < 8; bi++) {
            u8 b = buf[bi];
            if (st == 0) { if (b == 0x08u) { cp[0] = b; st = 1; } continue; }
            cp[st++] = b;
            if (st == 6) {
                st = 0; ncp++;
                if (cp[1] == 0x50u || cp[1] == 0x60u) {
                    nvcd++;
                    if (nvcd <= 6)
                        printf("[cpw] VCD reg=%02X val=%02X%02X%02X%02X  (cp writes so far %ld)\n",
                               cp[1], cp[2], cp[3], cp[4], cp[5], ncp);
                } else if (ncp <= 3) {
                    printf("[cpw] cp reg=%02X val=%02X%02X%02X%02X\n",
                           cp[1], cp[2], cp[3], cp[4], cp[5]);
                }
                fflush(stdout);
            }
        }
    }
    dol_gx_recomp_push_fifo(&g_gx, buf, sz);
    /* Own copy of this frame's command stream.
     *
     * gx_recomp's buffer is a diagnostic accumulator: capped at 262144 bytes,
     * spanning many frames, starting wherever it starts. Replaying that into
     * aurora landed mid-command and died on "unknown opcode 0x1C at pos 128".
     * aurora executes what it is given, so it needs exactly the bytes written
     * since the last draw-done, from a command boundary. */
    /* Byte-at-a-time so a CALL_DL command spanning several WGPIPE writes is
     * handled: swallow its 9 bytes and emit the referenced list in its place. */
    {   int cur = 0;
        /* Allocate here. The original append block -- which owned the
         * allocation -- was left disabled behind `if (0)` when this
         * byte-at-a-time loop replaced it, so g_seg[0] stayed NULL and every
         * append silently dropped. That is why 2.5 MB of captured GX traffic
         * reached aurora as 2,725 bytes. */
        if (!g_seg[cur]) {
            g_seg[cur] = (u8*)malloc(SEG_CAP + SEG_GUARD);
            if (g_seg[cur]) { memset(g_seg[cur] + SEG_CAP, 0xCC, SEG_GUARD); g_seg_cap[cur] = SEG_CAP; }
        }
        for (u8 bi = 0; bi < sz && bi < 8; bi++) {
            u8 b = buf[bi];
            if (s_swallow) { s_swallow--; continue; }
            if (b == 0x40 && s_pending_list && s_pending_len) {
                wgseg_inline_dl(s_pending_list, s_pending_len);
                s_pending_list = NULL; s_pending_len = 0;
                s_swallow = 8;                  /* drop the opcode's 8-byte payload */
                continue;
            }
            if (g_seg[cur] && g_seg_len[cur] + 1u <= g_seg_cap[cur])
                g_seg[cur][g_seg_len[cur]++] = b;
        }
    }
    if (0) {   int cur = g_seg_cur;
        if (!g_seg[cur]) {
            g_seg[cur] = (u8*)malloc(SEG_CAP + SEG_GUARD);
            if (g_seg[cur]) { memset(g_seg[cur] + SEG_CAP, 0xCC, SEG_GUARD); g_seg_cap[cur] = SEG_CAP; }
        }
        if (g_seg[cur] && g_seg_len[cur] + sz <= g_seg_cap[cur]) {
            memcpy(g_seg[cur] + g_seg_len[cur], buf, sz);
            g_seg_len[cur] += sz;
        }
    }
    g_wg_bytes += sz; g_wg_writes++;
    return true;
}
