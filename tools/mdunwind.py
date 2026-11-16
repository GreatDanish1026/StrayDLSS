"""x64 stack walk of a minidump's exception thread using .pdata/unwind info from the module files on
disk; symbol names for modules with a PDB in the given folder (dbghelp)."""
import bisect
import ctypes
import os
import struct
import sys
from ctypes import wintypes as W

path, pdbdirs = sys.argv[1], sys.argv[2:]
data = open(path, "rb").read()
_, _, n, d = struct.unpack_from("<IIII", data, 0)
st = {}
for i in range(n):
    t, s, r = struct.unpack_from("<III", data, d + i * 12)
    st.setdefault(t, []).append((s, r))


def u16(r):
    k = struct.unpack_from("<I", data, r)[0]
    return data[r + 4 : r + 4 + k].decode("utf-16-le")


mods = []
s, r = st[4][0]
for i in range(struct.unpack_from("<I", data, r)[0]):
    a = r + 4 + i * 108
    b, sz = struct.unpack_from("<QI", data, a)
    mods.append((b, sz, u16(struct.unpack_from("<I", data, a + 20)[0])))
ranges = []
if 9 in st:
    s, r = st[9][0]
    c, off = struct.unpack_from("<QQ", data, r)
    for i in range(c):
        a, sz = struct.unpack_from("<QQ", data, r + 16 + i * 16)
        ranges.append((a, sz, off))
        off += sz
if 5 in st:
    s, r = st[5][0]
    for i in range(struct.unpack_from("<I", data, r)[0]):
        a, sz, mrva = struct.unpack_from("<QII", data, r + 4 + i * 16)
        ranges.append((a, sz, mrva))


def mem(addr, n):
    for b, sz, o in ranges:
        if b <= addr and addr + n <= b + sz:
            return data[o + addr - b : o + addr - b + n]
    return None


def q(addr):
    b = mem(addr, 8)
    return struct.unpack("<Q", b)[0] if b else None


class Image:
    def __init__(self, name):
        self.f = open(name, "rb").read()
        f = self.f
        pe = struct.unpack_from("<I", f, 0x3C)[0]
        ns = struct.unpack_from("<H", f, pe + 6)[0]
        opt = struct.unpack_from("<H", f, pe + 20)[0]
        self.secs = [struct.unpack_from("<8sIIII", f, pe + 24 + opt + i * 40) for i in range(ns)]
        pd_rva, pd_size = struct.unpack_from("<II", f, pe + 24 + 112 + 3 * 8)  # data dir 3: exception
        self.funcs = []
        for k in range(pd_size // 12):
            self.funcs.append(struct.unpack("<III", self.at(pd_rva + k * 12, 12)))
        self.starts = [x[0] for x in self.funcs]

    def at(self, rva, n):
        for _, vs, va, rs, ro in self.secs:
            if va <= rva < va + max(vs, rs):
                o = ro + rva - va
                return self.f[o : o + n]
        return b"\0" * n

    def lookup(self, rva):
        i = bisect.bisect_right(self.starts, rva) - 1
        if i >= 0 and self.funcs[i][0] <= rva < self.funcs[i][1]:
            return self.funcs[i]
        return None


images = {}


def module(a):
    for b, sz, name in mods:
        if b <= a < b + sz:
            return b, name
    return None, None


REGS = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"] + ["r%d" % i for i in range(8, 16)]


def unwind(img, base, func, rip, regs):
    begin, end, info = func
    off = rip - base - begin
    while True:
        hdr = img.at(info, 4)
        ver_flags, prolog, count, fr = hdr
        flags = ver_flags >> 3
        frame_reg, frame_off = fr & 0xF, fr >> 4
        codes = img.at(info + 4, count * 2)
        # frame register: rsp = frame_reg - offset*16 (only if we're past the prolog's SET_FPREG)
        i = 0
        while i < count:
            code_off, opinfo = codes[i * 2], codes[i * 2 + 1]
            op, oi = opinfo & 0xF, opinfo >> 4
            applies = code_off <= off or info != func[2]  # chained infos always fully apply
            if op == 0:  # PUSH_NONVOL
                if applies:
                    regs[REGS[oi]] = q(regs["rsp"])
                    regs["rsp"] += 8
                i += 1
            elif op == 1:  # ALLOC_LARGE
                if oi == 0:
                    size = struct.unpack_from("<H", codes, (i + 1) * 2)[0] * 8
                    i += 2
                else:
                    size = struct.unpack_from("<I", codes, (i + 1) * 2)[0]
                    i += 3
                if applies:
                    regs["rsp"] += size
            elif op == 2:  # ALLOC_SMALL
                if applies:
                    regs["rsp"] += oi * 8 + 8
                i += 1
            elif op == 3:  # SET_FPREG
                if applies:
                    regs["rsp"] = regs[REGS[frame_reg]] - frame_off * 16
                i += 1
            elif op == 4:  # SAVE_NONVOL
                o = struct.unpack_from("<H", codes, (i + 1) * 2)[0] * 8
                if applies:
                    regs[REGS[oi]] = q(regs["rsp"] + o)
                i += 2
            elif op == 5:  # SAVE_NONVOL_FAR
                o = struct.unpack_from("<I", codes, (i + 1) * 2)[0]
                if applies:
                    regs[REGS[oi]] = q(regs["rsp"] + o)
                i += 3
            elif op == 8:  # SAVE_XMM128
                i += 2
            elif op == 9:  # SAVE_XMM128_FAR
                i += 3
            elif op == 10:  # PUSH_MACHFRAME
                if applies:
                    rsp = regs["rsp"] + (8 if oi else 0)
                    regs["rip_override"] = q(rsp)
                    regs["rsp"] = q(rsp + 24)
                i += 1
            else:
                i += 1
        if flags & 4:  # CHAININFO
            ci = info + 4 + ((count + 1) & ~1) * 2
            begin, end, info = struct.unpack("<III", img.at(ci, 12))
            continue
        break


dbg = ctypes.WinDLL("dbghelp.dll")
h = W.HANDLE(0x777)
dbg.SymSetOptions(0x2)
dbg.SymInitializeW(h, ";".join(pdbdirs), False)
dbg.SymLoadModuleExW.restype = ctypes.c_uint64
dbg.SymLoadModuleExW.argtypes = [W.HANDLE, W.HANDLE, W.LPCWSTR, W.LPCWSTR, ctypes.c_uint64, W.DWORD, ctypes.c_void_p, W.DWORD]
for b, sz, name in mods:
    if os.path.exists(name):
        dbg.SymLoadModuleExW(h, None, name, None, b, sz, None, 0)


class SI(ctypes.Structure):
    _fields_ = [("SizeOfStruct", W.ULONG), ("TypeIndex", W.ULONG), ("Reserved", ctypes.c_uint64 * 2), ("Index", W.ULONG),
                ("Size", W.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", W.ULONG), ("Value", ctypes.c_uint64),
                ("Address", ctypes.c_uint64), ("Register", W.ULONG), ("Scope", W.ULONG), ("Tag", W.ULONG),
                ("NameLen", W.ULONG), ("MaxNameLen", W.ULONG), ("Name", ctypes.c_char * 512)]


def name_of(a):
    b, name = module(a)
    if not b:
        return hex(a)
    si = SI()
    si.SizeOfStruct, si.MaxNameLen = 88, 500
    disp = ctypes.c_uint64()
    s = "%s+0x%x" % (os.path.basename(name), a - b)
    if dbg.SymFromAddr(h, ctypes.c_uint64(a), ctypes.byref(disp), ctypes.byref(si)):
        s += "  (%s+0x%x)" % (si.Name.decode(errors="replace"), disp.value)
    return s


s, r = st[6][0]
tid = struct.unpack_from("<I", data, r)[0]
ctx_size, ctx_rva = struct.unpack_from("<II", data, r + 8 + 152)
c = data[ctx_rva : ctx_rva + ctx_size]
regs = {REGS[i]: struct.unpack_from("<Q", c, 0x78 + i * 8)[0] for i in range(16)}
rip = struct.unpack_from("<Q", c, 0xF8)[0]
print("thread", tid)
for depth in range(60):
    print("%2d %s" % (depth, name_of(rip)))
    b, name = module(rip)
    if not b:
        break
    if name not in images:
        try:
            images[name] = Image(name)
        except Exception as e:
            print("   (can't read %s: %s)" % (name, e))
            break
    img = images[name]
    func = img.lookup(rip - b)
    if func:
        regs.pop("rip_override", None)
        unwind(img, b, func, rip, regs)
        if "rip_override" in regs:
            rip = regs.pop("rip_override")
            continue
    nxt = q(regs["rsp"])
    if nxt is None:
        print("   (stack not in dump at 0x%x)" % regs["rsp"])
        break
    regs["rsp"] += 8
    rip = nxt
