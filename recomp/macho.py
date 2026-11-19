"""Read-only parser for the thin armv7 Mach-O of GT Racing Motor Academy HD.

Only what the recompiler needs: segments, sections, symbols, function starts
(with the Thumb bit ld64 puts in bit 0), the indirect symbol table that names
each stub / lazy pointer, and helpers to read bytes and words by address.
"""
import struct

LC_SEGMENT = 0x1
LC_SYMTAB = 0x2
LC_UNIXTHREAD = 0x5
LC_DYSYMTAB = 0xB
LC_LOAD_DYLIB = 0xC
LC_ENCRYPTION_INFO = 0x21
LC_DYLD_INFO = 0x22
LC_DYLD_INFO_ONLY = 0x80000022
LC_FUNCTION_STARTS = 0x26

N_STAB = 0xE0
N_TYPE = 0x0E
N_UNDF = 0x0
N_SECT = 0xE
N_ARM_THUMB_DEF = 0x8

S_SYMBOL_STUBS = 0x8
S_LAZY_SYMBOL_POINTERS = 0x7
S_NON_LAZY_SYMBOL_POINTERS = 0x6
S_ZEROFILL = 0x1
INDIRECT_SYMBOL_LOCAL = 0x80000000
INDIRECT_SYMBOL_ABS = 0x40000000


class Section:
    def __init__(self, seg, name, addr, size, offset, flags, r1, r2):
        self.seg, self.name, self.addr, self.size = seg, name, addr, size
        self.offset, self.flags, self.reserved1, self.reserved2 = offset, flags, r1, r2

    @property
    def end(self):
        return self.addr + self.size

    @property
    def type(self):
        return self.flags & 0xFF

    def __repr__(self):
        return '%s,%s %08x-%08x' % (self.seg, self.name, self.addr, self.end)


class Segment:
    def __init__(self, name, vmaddr, vmsize, fileoff, filesize, prot):
        self.name, self.vmaddr, self.vmsize = name, vmaddr, vmsize
        self.fileoff, self.filesize, self.prot = fileoff, filesize, prot


class MachO:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.data = f.read()
        b = self.data
        magic, cpu, sub, ftype, ncmds, _, self.flags = struct.unpack_from('<7I', b, 0)
        if magic != 0xFEEDFACE or cpu != 12:
            raise ValueError('not a thin 32-bit ARM Mach-O')
        self.cpusubtype = sub
        self.segments, self.sections, self.dylibs = [], [], []
        self.entry = None
        self.encryption = None
        symtab = dysymtab = fstarts = None
        o = 28
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<2I', b, o)
            if cmd == LC_SEGMENT:
                name = b[o + 8:o + 24].rstrip(b'\0').decode()
                vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, _ = \
                    struct.unpack_from('<8I', b, o + 24)
                self.segments.append(Segment(name, vmaddr, vmsize, fileoff, filesize, initprot))
                so = o + 56
                for _ in range(nsects):
                    sn = b[so:so + 16].rstrip(b'\0').decode()
                    sg = b[so + 16:so + 32].rstrip(b'\0').decode()
                    addr, ssize, off, _a, _ro, _nr, fl, r1, r2 = struct.unpack_from('<9I', b, so + 32)
                    self.sections.append(Section(sg, sn, addr, ssize, off, fl, r1, r2))
                    so += 68
            elif cmd == LC_SYMTAB:
                symtab = struct.unpack_from('<4I', b, o + 8)
            elif cmd == LC_DYSYMTAB:
                dysymtab = struct.unpack_from('<18I', b, o + 8)
            elif cmd == LC_FUNCTION_STARTS:
                fstarts = struct.unpack_from('<2I', b, o + 8)
            elif cmd == LC_UNIXTHREAD:
                flavor, count = struct.unpack_from('<2I', b, o + 8)
                regs = struct.unpack_from('<%dI' % count, b, o + 16)
                self.entry = regs[15]
                self.entry_cpsr = regs[16] if count > 16 else 0
            elif cmd == LC_LOAD_DYLIB:
                noff = struct.unpack_from('<I', b, o + 8)[0]
                self.dylibs.append(b[o + noff:o + size].split(b'\0')[0].decode())
            elif cmd == LC_ENCRYPTION_INFO:
                self.encryption = struct.unpack_from('<3I', b, o + 8)
            o += size
        self._read_symbols(symtab)
        self._read_indirect(dysymtab)
        self._read_function_starts(fstarts)

    # address helpers
    def section(self, seg, name):
        for s in self.sections:
            if s.seg == seg and s.name == name:
                return s
        return None

    def section_at(self, addr):
        for s in self.sections:
            if s.addr <= addr < s.end:
                return s
        return None

    def file_offset(self, addr):
        for g in self.segments:
            if g.vmaddr <= addr < g.vmaddr + g.filesize:
                return addr - g.vmaddr + g.fileoff
        return None

    def read(self, addr, n):
        off = self.file_offset(addr)
        if off is None:
            s = self.section_at(addr)
            if s is not None and s.type == S_ZEROFILL:
                return bytes(n)
            raise ValueError('address %08x not in file' % addr)
        return self.data[off:off + n]

    def u32(self, addr):
        return struct.unpack('<I', self.read(addr, 4))[0]

    def u16(self, addr):
        return struct.unpack('<H', self.read(addr, 2))[0]

    def u8(self, addr):
        return self.read(addr, 1)[0]

    # tables
    def _read_symbols(self, symtab):
        b = self.data
        symoff, nsyms, stroff, _ = symtab
        self.symbols = []          # (name, type, sect, desc, value)
        for i in range(nsyms):
            strx, ntype, nsect, ndesc, nval = struct.unpack_from('<IBBHI', b, symoff + i * 12)
            end = b.index(b'\0', stroff + strx)
            self.symbols.append((b[stroff + strx:end].decode('latin1'), ntype, nsect, ndesc, nval))
        self.names = {}            # addr -> name (defined, non-stab)
        for name, t, s, d, v in self.symbols:
            if t & N_STAB or (t & N_TYPE) != N_SECT:
                continue
            self.names.setdefault(v, name)
        self.imports = [n for n, t, s, d, v in self.symbols
                        if not t & N_STAB and (t & N_TYPE) == N_UNDF]

    def _read_indirect(self, dysymtab):
        """Map stub addresses and lazy / non-lazy pointer slots to import names."""
        b = self.data
        indoff, nind = dysymtab[12], dysymtab[13]
        ind = struct.unpack_from('<%dI' % nind, b, indoff)
        self.stubs = {}            # stub addr -> name
        self.ptr_slots = {}        # pointer slot addr -> name (None = local)
        for s in self.sections:
            t = s.type
            if t == S_SYMBOL_STUBS:
                n = s.size // s.reserved2
                for i in range(n):
                    self.stubs[s.addr + i * s.reserved2] = self._ind_name(ind[s.reserved1 + i])
            elif t in (S_LAZY_SYMBOL_POINTERS, S_NON_LAZY_SYMBOL_POINTERS):
                for i in range(s.size // 4):
                    self.ptr_slots[s.addr + i * 4] = self._ind_name(ind[s.reserved1 + i])

    def _ind_name(self, idx):
        if idx & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS):
            return None
        return self.symbols[idx][0]

    def _read_function_starts(self, fstarts):
        """Return list of (addr, is_thumb); ld64 sets bit 0 for Thumb functions."""
        b = self.data
        off, size = fstarts
        text = next(g for g in self.segments if g.name == '__TEXT')
        addr = text.vmaddr
        i, end = off, off + size
        self.function_starts = []
        while i < end:
            v = sh = 0
            while True:
                c = b[i]
                i += 1
                v |= (c & 0x7F) << sh
                sh += 7
                if c < 0x80:
                    break
            if v == 0:
                break
            addr += v
            self.function_starts.append((addr & ~1, bool(addr & 1)))
