"""PE image loading: sections, raw memory view, and import table."""
import struct
import pefile


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path)
        oh = self.pe.OPTIONAL_HEADER
        self.base = oh.ImageBase
        self.entry = self.base + oh.AddressOfEntryPoint
        self.size = oh.SizeOfImage
        # Flat memory image of the whole module, as the Windows loader maps it.
        self.mem = bytearray(self.size)
        hdr = self.pe.get_data(0, oh.SizeOfHeaders)
        self.mem[0:len(hdr)] = hdr
        self.sections = []
        for s in self.pe.sections:
            name = s.Name.rstrip(b'\0').decode()
            va = self.base + s.VirtualAddress
            data = s.get_data()
            vsize = max(s.Misc_VirtualSize, len(data))
            self.mem[s.VirtualAddress:s.VirtualAddress + len(data)] = data
            self.sections.append((name, va, va + vsize, s.Characteristics))
        text = [s for s in self.sections if s[0] == '.text'][0]
        self.text_start, self.text_end = text[1], text[2]

        # Imports: IAT slot address -> (dll, name)
        self.imports = {}
        for entry in self.pe.DIRECTORY_ENTRY_IMPORT:
            dll = entry.dll.decode().upper()
            for imp in entry.imports:
                name = imp.name.decode() if imp.name else f'ord{imp.ordinal}'
                self.imports[imp.address] = (dll, name)

    def in_text(self, a):
        return self.text_start <= a < self.text_end

    def in_image(self, a):
        return self.base <= a < self.base + self.size

    def read(self, a, n):
        o = a - self.base
        return bytes(self.mem[o:o + n])

    def u8(self, a):
        return self.mem[a - self.base]

    def u32(self, a):
        return struct.unpack_from('<I', self.mem, a - self.base)[0]

    def section_of(self, a):
        for s in self.sections:
            if s[1] <= a < s[2]:
                return s[0]
        return None
