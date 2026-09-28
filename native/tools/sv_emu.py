#!/usr/bin/env python3
"""sv_emu.py — the whole original engine under Unicorn. DEVELOPMENT ONLY.

Maps TIBASE32 and the language DLLs at their preferred bases, stubs the
CRT/Win32 imports in Python, runs the DLLs' own entry points, and exposes
SVOpenSpeech / SVTextToPhon / SVNarrate. waveOutWrite buffers are captured and
the MM_WOM_DONE messages that drive rendering are pumped back into the
engine's window procedure, so a whole utterance renders.

`Emu.hooks` maps a code address to a callback; the verifiers use it to snapshot
the engine at stage boundaries inside FUN_1c003870.

Needs unicorn and pefile. The DLLs are the user's own copies; this is a test
oracle and nothing here is linked into, or shipped with, the library.
"""
import struct, sys
import pefile
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UC_PROT_ALL
from unicorn.x86_const import *

import os
STUB = 0x10000000
HEAP = 0x20000000
HEAP_SIZE = 0x04000000
STACK = 0x0F000000
STACK_SIZE = 0x00100000
DATA = 0x00100000
SENTINEL = 0x0FFFF000
HWND = 0x00001234
HWAVE = 0x00005000

STDCALL = {
    # KERNEL32
    "GlobalUnlock": 4, "GetVersion": 0, "GlobalFree": 4, "GlobalAlloc": 8, "GlobalLock": 4,
    "LoadLibraryA": 4, "GetProcAddress": 8, "FreeLibrary": 4, "LocalAlloc": 8, "LocalFree": 4,
    # USER32
    "PostMessageA": 16, "DestroyWindow": 4, "CreateWindowExA": 48, "RegisterClassA": 4,
    "PeekMessageA": 20, "RegisterWindowMessageA": 4, "TranslateMessage": 4,
    "DefWindowProcA": 16, "DispatchMessageA": 4,
    # WINMM
    "timeGetTime": 0, "waveOutUnprepareHeader": 12, "waveOutWrite": 12, "timeBeginPeriod": 4,
    "waveOutPrepareHeader": 12, "waveOutClose": 4, "timeEndPeriod": 4, "timeKillEvent": 4,
    "waveOutOpen": 24, "waveOutGetNumDevs": 0, "waveOutPause": 4, "waveOutRestart": 4,
    "waveOutReset": 4, "timeSetEvent": 20,
}


class Emu:
    def __init__(self, dll_dir, languages=("TIENG32.DLL",), trace=False):
        self.dll_dir = dll_dir
        self.mu = mu = Uc(UC_ARCH_X86, UC_MODE_32)
        mu.mem_map(STUB, 0x10000)
        mu.mem_map(HEAP, HEAP_SIZE)
        mu.mem_map(STACK, STACK_SIZE)
        mu.mem_map(DATA, 0x100000)
        mu.mem_map(SENTINEL, 0x1000)
        mu.mem_write(SENTINEL, b"\xf4")  # hlt
        self.heap_top = HEAP + 0x10
        self.blocks = {}
        self.stubs = {}
        self.data_top = DATA
        self.images = {}
        self.posted = []
        self.wave = []          # captured PCM chunks
        self.timers = {}
        self.time = 0
        self.stop_hooks = {}
        self.trace = trace
        self.coverage = set()
        self.hooks = {}

        # CRT data: ctype table for the "C" locale, _pctype pointer, mb_cur_max
        tbl = bytearray(257 * 2)
        for c in range(256):
            v = 0
            if 0x41 <= c <= 0x5a: v |= 0x1 | 0x100
            if 0x61 <= c <= 0x7a: v |= 0x2 | 0x100
            if 0x30 <= c <= 0x39: v |= 0x4
            if c in (0x20, 0x09, 0x0a, 0x0b, 0x0c, 0x0d): v |= 0x8
            if 0x21 <= c <= 0x2f or 0x3a <= c <= 0x40 or 0x5b <= c <= 0x60 or 0x7b <= c <= 0x7e: v |= 0x10
            if c < 0x20 or c == 0x7f: v |= 0x20
            if c in (0x20, 0x09): v |= 0x40   # MSVCRT: _BLANK on space and tab
            if (0x30 <= c <= 0x39) or (0x41 <= c <= 0x46) or (0x61 <= c <= 0x66): v |= 0x80
            struct.pack_into("<H", tbl, 2 + 2 * c, v)
        self.ctype = [struct.unpack_from("<H", tbl, 2 + 2 * c)[0] for c in range(256)]
        t = self.alloc_data(bytes(tbl))
        self.pctype_ptr = self.alloc_data(struct.pack("<I", t + 2))
        self.mb_cur_max = self.alloc_data(struct.pack("<I", 1))
        self.adjust_fdiv = self.alloc_data(struct.pack("<I", 0))
        self.tm_buf = self.alloc_data(bytes(64))

        self.load("TIBASE32.DLL")
        for l in languages:
            self.load(l)
        mu.hook_add(UC_HOOK_CODE, self._stub_hook, begin=STUB, end=STUB + 0xffff)
        mu.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        if trace:
            mu.hook_add(UC_HOOK_CODE, self._trace, begin=0x1c000000, end=0x1c5fffff)
        mu.hook_add(UC_HOOK_CODE, self._stage_hook, begin=0x1c003870, end=0x1c003da0)
        # DLL entry points, DLL_PROCESS_ATTACH
        for name, img in self.images.items():
            ep = img["base"] + img["pe"].OPTIONAL_HEADER.AddressOfEntryPoint
            r = self.call(ep, [img["base"], 1, 0], stdcall=True)
            assert r, "%s DllMain failed" % name

    # ---- memory helpers ----
    def alloc_data(self, b):
        a = (self.data_top + 15) & ~15
        self.mu.mem_write(a, bytes(b))
        self.data_top = a + len(b)
        return a

    def malloc(self, n, zero=True):
        n = max(n, 1)
        a = (self.heap_top + 15) & ~15
        self.heap_top = a + n + 16
        assert self.heap_top < HEAP + HEAP_SIZE, "heap exhausted"
        self.blocks[a] = n
        # heap memory starts zeroed; calloc and malloc look the same, which
        # is fine for an oracle only if the engine never reads uninitialised
        # heap — flagged by filling malloc blocks with a pattern instead.
        if not zero:
            self.mu.mem_write(a, b"\xcd" * n)
        return a

    def rd(self, a, n): return bytes(self.mu.mem_read(a, n))
    def u8(self, a): return self.rd(a, 1)[0]
    def u16(self, a): return struct.unpack("<H", self.rd(a, 2))[0]
    def s16(self, a): return struct.unpack("<h", self.rd(a, 2))[0]
    def u32(self, a): return struct.unpack("<I", self.rd(a, 4))[0]
    def w32(self, a, v): self.mu.mem_write(a, struct.pack("<I", v & 0xffffffff))
    def cstr(self, a, maxn=100000):
        out = bytearray()
        while len(out) < maxn:
            c = self.u8(a + len(out))
            if c == 0: break
            out.append(c)
        return bytes(out)

    # ---- image loading ----
    def load(self, name):
        pe = pefile.PE(os.path.join(self.dll_dir, name))
        base = pe.OPTIONAL_HEADER.ImageBase
        size = (pe.OPTIONAL_HEADER.SizeOfImage + 0xfff) & ~0xfff
        self.mu.mem_map(base, size, UC_PROT_ALL)
        self.mu.mem_write(base, pe.get_memory_mapped_image())
        exports = {}
        if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
            for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
                exports[e.name.decode()] = base + e.address
        for d in pe.DIRECTORY_ENTRY_IMPORT:
            for i in d.imports:
                nm = i.name.decode()
                if nm == "_adjust_fdiv":
                    self.w32(i.address, self.adjust_fdiv)
                    continue
                if nm not in self.stubs.values() or True:
                    addr = STUB + 16 * len(self.stubs)
                    self.stubs[addr] = nm
                    n = STDCALL.get(nm)
                    code = b"\xc3" if not n else b"\xc2" + struct.pack("<H", n)
                    self.mu.mem_write(addr, code)
                    self.w32(i.address, addr)
        self.images[name] = {"pe": pe, "base": base, "exports": exports}

    # ---- calling into emulated code ----
    def call(self, fn, args, stdcall=False, until=None, count=0):
        mu = self.mu
        esp = self._esp if hasattr(self, "_esp") else STACK + STACK_SIZE - 0x1000
        saved = esp
        for a in reversed(args):
            esp -= 4
            self.w32(esp, a)
        esp -= 4
        self.w32(esp, SENTINEL)
        mu.reg_write(UC_X86_REG_ESP, esp)
        self._esp = esp - 0x100   # nested calls go below
        try:
            mu.emu_start(fn, until if until else SENTINEL, count=count)
        except UcError as e:
            eip = mu.reg_read(UC_X86_REG_EIP)
            raise RuntimeError("emulation fault at %08x: %s" % (eip, e))
        finally:
            self._esp = saved if saved != STACK + STACK_SIZE - 0x1000 else None
            if self._esp is None:
                del self._esp
        return mu.reg_read(UC_X86_REG_EAX)

    def _unmapped(self, mu, access, addr, size, value, ud):
        eip = mu.reg_read(UC_X86_REG_EIP)
        print("UNMAPPED access %d at %08x size %d (eip %08x)" % (access, addr, size, eip), file=sys.stderr)
        return False

    def _stage_hook(self, mu, addr, size, ud):
        cb = self.hooks.get(addr)
        if cb:
            cb(self)

    def _trace(self, mu, addr, size, ud):
        self.coverage.add(addr)

    def arg(self, k):
        esp = self.mu.reg_read(UC_X86_REG_ESP)
        return self.u32(esp + 4 + 4 * k)

    def _stub_hook(self, mu, addr, size, ud):
        nm = self.stubs.get(addr)
        if nm is None:
            return
        fn = getattr(self, "api_" + nm, None)
        if fn is None:
            raise RuntimeError("unimplemented import %s" % nm)
        r = fn()
        if r is not None:
            mu.reg_write(UC_X86_REG_EAX, r & 0xffffffff)

    # ---- CRT ----
    def api_malloc(self): return self.malloc(self.arg(0), zero=False)
    def api_calloc(self):
        n = self.arg(0) * self.arg(1)
        return self.malloc(n)
    def api_realloc(self):
        p, n = self.arg(0), self.arg(1)
        q = self.malloc(n, zero=False)
        if p:
            old = self.blocks.get(p, 0)
            self.mu.mem_write(q, self.rd(p, min(old, n)))
        return q
    def api_free(self): return 0
    def api__initterm(self):
        a, b = self.arg(0), self.arg(1)
        while a < b:
            if self.u32(a):
                raise RuntimeError("_initterm with a live initializer at %08x" % a)
            a += 4
        return 0
    def api_toupper(self):
        c = self.arg(0)
        return c - 32 if 0x61 <= c <= 0x7a else c
    def api_tolower(self):
        c = self.arg(0)
        return c + 32 if 0x41 <= c <= 0x5a else c
    def api___p___mb_cur_max(self): return self.mb_cur_max
    def api___p__pctype(self): return self.pctype_ptr
    def api__isctype(self):
        c, m = self.arg(0), self.arg(1)
        return self.ctype[c & 0xff] & m if 0 <= c < 256 else 0
    def api_strncpy(self):
        d, s, n = self.arg(0), self.arg(1), self.arg(2)
        src = self.cstr(s, n)
        self.mu.mem_write(d, src + b"\0" * (n - len(src)))
        return d
    def api_strncat(self):
        d, s, n = self.arg(0), self.arg(1), self.arg(2)
        dl = len(self.cstr(d))
        src = self.cstr(s, n)
        self.mu.mem_write(d + dl, src + b"\0")
        return d
    def api_strncmp(self):
        a, b, n = self.arg(0), self.arg(1), self.arg(2)
        for k in range(n):
            x, y = self.u8(a + k), self.u8(b + k)
            if x != y: return (x - y)
            if x == 0: return 0
        return 0
    def api_strchr(self):
        s, c = self.arg(0), self.arg(1) & 0xff
        k = 0
        while True:
            x = self.u8(s + k)
            if x == c: return s + k
            if x == 0: return 0
            k += 1
    def api_strrchr(self):
        s, c = self.arg(0), self.arg(1) & 0xff
        st = self.cstr(s)
        if c == 0: return s + len(st)
        i = st.rfind(bytes([c]))
        return s + i if i >= 0 else 0
    def api_strspn(self):
        s, a = self.cstr(self.arg(0)), self.cstr(self.arg(1))
        k = 0
        while k < len(s) and s[k] in a: k += 1
        return k
    def api_strcspn(self):
        s, a = self.cstr(self.arg(0)), self.cstr(self.arg(1))
        k = 0
        while k < len(s) and s[k] not in a: k += 1
        return k
    def api__ltoa(self):
        v, buf, radix = self.arg(0), self.arg(1), self.arg(2)
        assert radix == 10
        v = v - (1 << 32) if v & 0x80000000 else v
        self.mu.mem_write(buf, str(v).encode() + b"\0")
        return buf
    def api_time(self):
        p = self.arg(0)
        if p: self.w32(p, 0x32a9a200)
        return 0x32a9a200
    def api_localtime(self): return self.tm_buf

    # ---- virtual files (opt-in; used to exercise SVLoadUserDictionary, which
    # reads through fopen/fread/fclose rather than a Win32 file API) ----
    def register_file(self, path, data):
        """Make fopen(path, ...) succeed and fread() serve `data`."""
        if not hasattr(self, "_vfiles"):
            self._vfiles, self._open_files, self._next_fh = {}, {}, 1
        self._vfiles[path] = bytes(data)

    def api_fopen(self):
        path = self.cstr(self.arg(0)).decode("latin-1")
        vfiles = getattr(self, "_vfiles", {})
        if path not in vfiles:
            return 0
        fh = self._next_fh
        self._next_fh += 1
        self._open_files[fh] = {"data": vfiles[path], "pos": 0}
        return fh

    def api_fclose(self):
        stream = self.arg(0)
        return 0 if getattr(self, "_open_files", {}).pop(stream, None) is not None else -1 & 0xffffffff

    def api_fread(self):
        buf, size, count, stream = self.arg(0), self.arg(1), self.arg(2), self.arg(3)
        f = getattr(self, "_open_files", {}).get(stream)
        if not f or size == 0:
            return 0
        n = size * count
        chunk = f["data"][f["pos"]:f["pos"] + n]
        chunk = chunk[:len(chunk) - (len(chunk) % size)]
        if chunk:
            self.mu.mem_write(buf, chunk)
            f["pos"] += len(chunk)
        return len(chunk) // size

    # ---- KERNEL32 ----
    def api_GetVersion(self): return 0x0a280105  # NT-like: high bit clear
    def api_GlobalAlloc(self): return self.malloc(self.arg(1))
    def api_GlobalLock(self): return self.arg(0)
    def api_GlobalUnlock(self): return 1
    def api_GlobalFree(self): return 0
    def api_LocalAlloc(self): return self.malloc(self.arg(1))
    def api_LocalFree(self): return 0
    def api_LoadLibraryA(self):
        nm = self.cstr(self.arg(0)).decode().upper()
        for k, img in self.images.items():
            if k.upper() == nm:
                return img["base"]
        return 0
    def api_GetProcAddress(self):
        h, p = self.arg(0), self.arg(1)
        nm = self.cstr(p).decode()
        for img in self.images.values():
            if img["base"] == h:
                return img["exports"].get(nm, 0)
        return 0
    def api_FreeLibrary(self): return 1

    # ---- USER32 ----
    def api_RegisterClassA(self):
        wc = self.arg(0)
        self.wndproc = self.u32(wc + 4)
        return 0xc001
    def api_RegisterWindowMessageA(self): return 0xc100
    def api_CreateWindowExA(self): return HWND
    def api_DestroyWindow(self): return 1
    def api_PostMessageA(self):
        self.posted.append((self.arg(0), self.arg(1), self.arg(2), self.arg(3)))
        return 1
    def api_PeekMessageA(self): return 0
    def api_TranslateMessage(self): return 0
    def api_DispatchMessageA(self): return 0
    def api_DefWindowProcA(self): return 0

    # ---- WINMM ----
    def api_timeGetTime(self):
        self.time += 1
        return self.time
    def api_timeBeginPeriod(self): return 0
    def api_timeEndPeriod(self): return 0
    def api_timeSetEvent(self):
        tid = 7 + len(self.timers)
        self.timers[tid] = (self.arg(2), self.arg(3))
        return tid
    def api_timeKillEvent(self):
        self.timers.pop(self.arg(0), None)
        return 0
    def api_waveOutGetNumDevs(self): return 1
    def api_waveOutOpen(self):
        p = self.arg(0)
        if p: self.w32(p, HWAVE)
        return 0
    def api_waveOutClose(self): return 0
    def api_waveOutPrepareHeader(self):
        h = self.arg(1)
        self.w32(h + 0x10, self.u32(h + 0x10) | 2)
        return 0
    def api_waveOutUnprepareHeader(self):
        h = self.arg(1)
        self.w32(h + 0x10, self.u32(h + 0x10) & ~2)
        return 0
    def api_waveOutWrite(self):
        h = self.arg(1)
        data, n = self.u32(h), self.u32(h + 4)
        self.wave.append(self.rd(data, n))
        self.done_queue.append(h)
        # played instantly: WHDR_DONE, not INQUEUE
        self.w32(h + 0x10, (self.u32(h + 0x10) | 1) & ~0x10)
        return 0
    def api_waveOutReset(self): return 0
    def api_waveOutPause(self): return 0
    def api_waveOutRestart(self): return 0

    # ---- public API ----
    def exp(self, name):
        return self.images["TIBASE32.DLL"]["exports"][name]

    def open(self, lang=1):
        out = self.malloc(4)
        r = self.call(self.exp("_SVOpenSpeech@20"), [out, 0, 0xffffffff, lang, 0])
        if r:
            raise RuntimeError("SVOpenSpeech -> %#x" % r)
        self.h = self.u32(out)
        return self.h

    def text_to_phon(self, text, flags=0, cap=4096):
        t = self.malloc(len(text) + 1)
        self.mu.mem_write(t, text + b"\0")
        buf = self.malloc(cap)
        r = self.call(self.exp("_SVTextToPhon@24"), [self.h, t, buf, cap, flags, 0])
        return r, self.cstr(buf)

    def narrate(self, phon, flags=0, pump=True, max_ticks=100000):
        """Run SVNarrate; returns captured PCM (bytes)."""
        p = self.malloc(len(phon) + 1)
        self.mu.mem_write(p, phon + b"\0")
        self.wave = []
        self.posted = []
        self.done_queue = []
        r = self.call(self.exp("_SVNarrate@20"), [self.h, p, HWND, flags, 0])
        if r:
            raise RuntimeError("SVNarrate -> %#x" % r)
        ticks = 0
        while pump and self.done_queue and ticks < max_ticks:
            h = self.done_queue.pop(0)
            self.call(self.wndproc, [HWND, 0x3bd, HWAVE, h])
            ticks += 1
        return b"".join(self.wave)


if __name__ == "__main__":
    import sys
    e = Emu(sys.argv[2] if len(sys.argv) > 2 else ".")
    e.open()
    text = (sys.argv[1] if len(sys.argv) > 1 else "hello world").encode("latin-1")
    r, ph = e.text_to_phon(text)
    print("TextToPhon ->", r, ph)
    pcm = e.narrate(ph)
    print("PCM bytes:", len(pcm), "timers left:", e.timers, "posted:", [(hex(m), hex(w)) for (_, m, w, _) in e.posted][:10])
