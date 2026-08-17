"""Read a Windows minidump and report what faulted, and where.

Enough of the format to answer the only questions that matter after a crash:
which exception, at what address, and in whose module - so the address can be
turned into an RVA and looked up in our own notes.

    python tools/dump_info.py C:\\path\\to\\crash.dmp
"""
import struct
import sys

STREAM_MODULE_LIST = 4
STREAM_EXCEPTION = 6

EXC = {
    0xC0000005: "ACCESS_VIOLATION",
    0xC000001D: "ILLEGAL_INSTRUCTION",
    0xC0000025: "NONCONTINUABLE_EXCEPTION",
    0xC0000026: "INVALID_DISPOSITION",
    0xC000008C: "ARRAY_BOUNDS_EXCEEDED",
    0xC0000094: "INT_DIVIDE_BY_ZERO",
    0xC0000096: "PRIV_INSTRUCTION",
    0xC00000FD: "STACK_OVERFLOW",
    0xC0000409: "STACK_BUFFER_OVERRUN / FAST_FAIL",
    0xC0000374: "HEAP_CORRUPTION",
    0x80000003: "BREAKPOINT",
}


def mdstring(d, rva):
    if rva == 0 or rva + 4 > len(d):
        return "?"
    n = struct.unpack_from("<I", d, rva)[0]
    return d[rva + 4: rva + 4 + n].decode("utf-16-le", "replace")


def main():
    path = sys.argv[1]
    d = open(path, "rb").read()
    if d[:4] != b"MDMP":
        sys.exit("not a minidump")

    n_streams, dir_rva = struct.unpack_from("<II", d, 8)
    streams = {}
    for i in range(n_streams):
        t, size, rva = struct.unpack_from("<III", d, dir_rva + i * 12)
        streams[t] = (size, rva)

    # --- modules ---
    modules = []
    if STREAM_MODULE_LIST in streams:
        _, rva = streams[STREAM_MODULE_LIST]
        count = struct.unpack_from("<I", d, rva)[0]
        off = rva + 4
        for i in range(count):
            base, size, _cs, _ts, name_rva = struct.unpack_from("<QIIII"[:5], d, off)[:5] \
                if False else struct.unpack_from("<QIIII", d, off)[:5]
            modules.append((base, size, mdstring(d, name_rva)))
            off += 108

    def whose(addr):
        for base, size, name in modules:
            if base <= addr < base + size:
                short = name.split("\\")[-1]
                return "%s + 0x%X" % (short, addr - base)
        return "<unknown module>"

    # --- exception ---
    if STREAM_EXCEPTION not in streams:
        print("no exception stream")
        return
    _, rva = streams[STREAM_EXCEPTION]
    tid = struct.unpack_from("<I", d, rva)[0]
    tid_target = tid
    code, flags, rec, addr, nparams = struct.unpack_from("<IIQQI", d, rva + 8)
    params = struct.unpack_from("<15Q", d, rva + 8 + 4 + 4 + 8 + 8 + 4 + 4)

    print("thread          : %d" % tid)
    print("exception code  : 0x%08X  %s" % (code, EXC.get(code, "?")))
    print("exception addr  : 0x%016X   %s" % (addr, whose(addr)))
    if code == 0xC0000005 and nparams >= 2:
        kind = {0: "READ from", 1: "WRITE to", 8: "EXECUTE at"}.get(params[0], "access")
        print("access          : %s 0x%016X   %s" % (kind, params[1], whose(params[1])))
    # --- pseudo-stack of the faulting thread -------------------------------
    # No symbols and no unwind here, so instead scan the thread's stack memory
    # for values that land inside a known module's code. That over-reports (old
    # frames, spilled pointers) but it answers the question that matters after a
    # crash in a hooked game: WHOSE code is on this stack at all.
    STREAM_THREAD_LIST = 3
    if STREAM_THREAD_LIST in streams:
        _, trva = streams[STREAM_THREAD_LIST]
        tcount = struct.unpack_from("<I", d, trva)[0]
        off = trva + 4
        for i in range(tcount):
            tid, _susp, _pc, _pri, _teb, stack_start, stack_size, stack_rva = \
                struct.unpack_from("<IIIIQQII", d, off)
            off += 48
            if tid != tid_target:
                continue
            print()
            print("stack of the faulting thread (0x%X..0x%X, %d bytes):"
                  % (stack_start, stack_start + stack_size, stack_size))
            seen = []
            for p in range(0, stack_size - 8, 8):
                v = struct.unpack_from("<Q", d, stack_rva + p)[0]
                w = whose(v)
                if w == "<unknown module>":
                    continue
                if seen and seen[-1] == w:
                    continue
                seen.append(w)
                print("   +0x%05X  0x%016X   %s" % (p, v, w))
                if len(seen) > 60:
                    print("   ... (truncated)")
                    break
            break

    print()
    print("modules of interest:")
    for base, size, name in modules:
        short = name.split("\\")[-1].lower()
        if any(k in short for k in ("thehunter", "cotwvr", "xinput", "openxr", "d3d11", "dxgi")):
            print("   %-28s base 0x%016X  size 0x%X" % (name.split("\\")[-1], base, size))


main()
