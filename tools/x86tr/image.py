"""The prepared EXE image (a dcprepare cache) as guest-addressed bytes."""
import struct
import zlib

CACHE_MAGIC = b'DCUSAIMG'


class Section:
    def __init__(self, name, va, size, file_backed, characteristics):
        self.name, self.va, self.size, self.file_backed, self.characteristics = name, va, size, file_backed, characteristics

    @property
    def end(self):
        return self.va + self.size

    @property
    def executable(self):
        return bool(self.characteristics & 0x20000000)

    def __repr__(self):
        return '%s %08x..%08x' % (self.name, self.va, self.end)


class Image:
    def __init__(self, path):
        with open(path, 'rb') as f:
            raw = f.read()
        if raw[:8] != CACHE_MAGIC:
            raise ValueError('%s is not a prepared image (run dcprepare)' % path)
        version, edition, base, size, packed = struct.unpack_from('<IIIII', raw, 8)
        self.edition = edition
        self.base = base
        self.data = zlib.decompress(raw[64:64 + packed])
        if len(self.data) != size:
            raise ValueError('prepared image is damaged')
        self._parse_headers()

    def _parse_headers(self):
        pe = self.u32(self.base + 0x3C) + self.base
        count = self.u16(pe + 6)
        opt_size = self.u16(pe + 20)
        opt = pe + 24
        self.entry = self.base + self.u32(opt + 16)
        self.directories = [(self.u32(opt + 96 + 8 * i), self.u32(opt + 100 + 8 * i)) for i in range(16)]
        self.sections = []
        for i in range(count):
            s = opt + opt_size + 40 * i
            name = self.bytes(s, 8).rstrip(b'\0').decode('ascii')
            vsize, rva, raw_size = struct.unpack_from('<III', self.data, s + 8 - self.base)
            ch = self.u32(s + 36)
            self.sections.append(Section(name, self.base + rva, vsize, min(vsize, raw_size), ch))
        # Code is every executable section: .text, and in both editions a small
        # second one (PSFD00, about 8 KB of library code reached through data
        # pointers). `text` spans them all, so per-byte coverage stays one array;
        # in_text() is true only inside an executable section, not the padding
        # between them.
        self.code_sections = [s for s in self.sections if s.executable]
        first = min(s.va for s in self.code_sections)
        last = max(s.va + s.file_backed for s in self.code_sections)
        self.text = Section('.text', first, last - first, last - first, 0x60000020)

    def section_of(self, va):
        for s in self.sections:
            if s.va <= va < s.end:
                return s
        return None

    def contains(self, va, n=1):
        return self.base <= va and va + n <= self.base + len(self.data)

    def bytes(self, va, n):
        o = va - self.base
        return self.data[o:o + n]

    def u8(self, va):
        return self.data[va - self.base]

    def u16(self, va):
        return struct.unpack_from('<H', self.data, va - self.base)[0]

    def u32(self, va):
        return struct.unpack_from('<I', self.data, va - self.base)[0]

    def i32(self, va):
        return struct.unpack_from('<i', self.data, va - self.base)[0]

    def cstr(self, va, limit=512):
        o = va - self.base
        end = self.data.find(b'\0', o, o + limit)
        return self.data[o:end if end >= 0 else o + limit]

    def in_text(self, va):
        if not self.text.va <= va < self.text.va + self.text.file_backed:
            return False
        for s in self.code_sections:
            if s.va <= va < s.va + s.file_backed:
                return True
        return False

    def imports(self):
        """{iat slot va: (dll, name)} from the import directory."""
        rva, size = self.directories[1]
        out = {}
        if not rva:
            return out
        d = self.base + rva
        while True:
            ilt, _, _, name_rva, iat = struct.unpack_from('<IIIII', self.data, d - self.base)
            if not name_rva:
                break
            dll = self.cstr(self.base + name_rva).decode('ascii')
            thunks = ilt or iat
            i = 0
            while True:
                entry = self.u32(self.base + thunks + 4 * i)
                if not entry:
                    break
                if entry & 0x80000000:
                    fname = '#%d' % (entry & 0xFFFF)
                else:
                    fname = self.cstr(self.base + entry + 2).decode('ascii')
                out[self.base + iat + 4 * i] = (dll, fname)
                i += 1
            d += 20
        return out

    def load_config(self):
        rva, size = self.directories[10]
        if not rva:
            return None
        lc = self.base + rva
        return {'security_cookie': self.u32(lc + 60), 'se_handler_table': self.u32(lc + 64), 'se_handler_count': self.u32(lc + 68)}
