from pathlib import Path
import struct
root=Path(__file__).resolve().parents[1]
p=root/"native/host/gxrt/hle_gale01.c"
s=p.read_text()
start=s.index('    /* Block until the host actually presents',s.index('void func_8034F314'))
end=s.index('\n}\n',start)
s=s[:start]+'''    /* Wait for the SDK retrace counter to advance. Only the VI interrupt
     * dispatcher invokes __VIRetraceHandler; calling it here fabricated
     * hundreds of thousands of retraces per second. */
    uint32_t previous = mem_r32(0x804D7420u);
    while (mem_r32(0x804D7420u) == previous) {
        frontend_tick_guest_time();
        frontend_deliver_interrupts(ctx);
        arq_drain(ctx);
        Sleep(1);
    }
'''+s[end:]
# VIGetRetraceCount only observes the counter; ticks and IRQs are host-time gated.
start=s.index('    static long s_seen = -1;',s.index('void func_8035017C'))
end=s.index('\n}\n',start)
s=s[:start]+'''    frontend_tick_guest_time();
    frontend_deliver_interrupts(ctx);
    arq_drain(ctx);
    ctx->gpr[3] = mem_r32(0x804D7420u);
'''+s[end:]
p.write_text(s)
p=root/"native/host/gxrt/frontend_main.c"
s=p.read_text().replace('C:/Users/hyper/melee/orig/GALE01/sys/main.dol','data/GALE01/sys/main.dol').replace('C:/Users/hyper/melee/orig/GALE01/sys/fst.bin','data/GALE01/sys/fst.bin')
# CPU/interrupt state belongs exclusively to the guest thread.
s=s.replace('        frontend_tick_guest_time();\n        /* Pace at a real retrace', '        /* Pace at a real retrace')
# Asynchronous guest calls must preserve the complete interrupted register file.
start=s.index('    uint32_t save3 =',s.index('static void ctx_deliver'))
end=s.index('\n}',start)
s=s[:start]+'''    CPUState saved = *cpu;
    cpu->msr &= ~0x8000u;
    cpu->gpr[3] = which;
    cpu->gpr[4] = osctx;
    fn(cpu);
    uint64_t advanced_tb = cpu->timebase;
    *cpu = saved;
    cpu->timebase = advanced_tb;
'''+s[end:]
p.write_text(s)
p=root/"native/host/gxrt/aurora_link.c"
s=p.read_text().replace('build/shim_msvc/out/aurora_shim.dll','build/runtime-probe/build/shim_msvc/out/aurora_shim.dll')
p.write_text(s)
# FST is game disc data, obtained from the user's already-mounted image.
disc=Path(r"C:/Users/hyper/roms/GC/Super Smash Bros. Melee (USA) (En,Ja) (v1.02).iso")
with disc.open("rb") as f:
    f.seek(0x424)
    off,size=struct.unpack(">II",f.read(8))
    f.seek(off)
    (root/"data/GALE01/sys/fst.bin").write_bytes(f.read(size))
