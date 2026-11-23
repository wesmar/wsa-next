# WSA-Next © 2026 Marek Wesołowski - wesmar
"""Minimal DEX reader: class model + per-method code references.

No third-party dependencies. Parses just enough of the DEX format to:
  * list classes with superclass, interfaces, access flags, fields and methods
  * walk each method's bytecode and collect the type/field/method references it uses
"""
from __future__ import annotations

import struct
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

NO_INDEX = 0xFFFFFFFF

ACC_PUBLIC = 0x1
ACC_PRIVATE = 0x2
ACC_STATIC = 0x8
ACC_FINAL = 0x10
ACC_INTERFACE = 0x200
ACC_ABSTRACT = 0x400
ACC_CONSTRUCTOR = 0x10000

# Resource package ids whose inlined constants are recorded as ('const', (id,)) refs.
RES_PACKAGES = {0x01}


def _uleb(buf: bytes, off: int) -> tuple[int, int]:
    result = shift = 0
    while True:
        b = buf[off]
        off += 1
        result |= (b & 0x7F) << shift
        if b < 0x80:
            return result, off
        shift += 7


# Width in 16-bit code units and reference kind for every opcode.
# kind: None, 'type', 'field', 'method', 'string', 'callsite', 'mhandle', 'proto'
_OPS: list[tuple[int, str | None]] = [(1, None)] * 256


def _set(lo: int, hi: int, width: int, kind: str | None = None) -> None:
    for op in range(lo, hi + 1):
        _OPS[op] = (width, kind)


_set(0x00, 0x01, 1); _set(0x02, 0x02, 2); _set(0x03, 0x03, 3)
_set(0x04, 0x04, 1); _set(0x05, 0x05, 2); _set(0x06, 0x06, 3)
_set(0x07, 0x07, 1); _set(0x08, 0x08, 2); _set(0x09, 0x09, 3)
_set(0x0A, 0x12, 1)
_set(0x13, 0x13, 2); _set(0x14, 0x14, 3); _set(0x15, 0x16, 2); _set(0x17, 0x17, 3)
_set(0x18, 0x18, 5); _set(0x19, 0x19, 2)
_set(0x1A, 0x1A, 2, 'string'); _set(0x1B, 0x1B, 3, 'string')
_set(0x1C, 0x1C, 2, 'type'); _set(0x1D, 0x1E, 1); _set(0x1F, 0x1F, 2, 'type')
_set(0x20, 0x20, 2, 'type'); _set(0x21, 0x21, 1); _set(0x22, 0x23, 2, 'type')
_set(0x24, 0x25, 3, 'type'); _set(0x26, 0x26, 3); _set(0x27, 0x28, 1)
_set(0x29, 0x29, 2); _set(0x2A, 0x2C, 3)
_set(0x2D, 0x3D, 2); _set(0x3E, 0x43, 1)
_set(0x44, 0x51, 2)
_set(0x52, 0x6D, 2, 'field')
_set(0x6E, 0x72, 3, 'method'); _set(0x73, 0x73, 1); _set(0x74, 0x78, 3, 'method')
_set(0x79, 0x7A, 1)
_set(0x7B, 0x8F, 1); _set(0x90, 0xAF, 2); _set(0xB0, 0xCF, 1); _set(0xD0, 0xE2, 2)
_set(0xE3, 0xF9, 1)
_set(0xFA, 0xFB, 4, 'method'); _set(0xFC, 0xFD, 3, 'callsite')
_set(0xFE, 0xFE, 2, 'mhandle'); _set(0xFF, 0xFF, 2, 'proto')


@dataclass
class MethodDef:
    name: str
    proto: str          # "(Ljava/lang/String;I)V"
    access: int
    refs: list[tuple[str, tuple]] = field(default_factory=list)  # (kind, ref)

    @property
    def sig(self) -> str:
        return self.name + self.proto


@dataclass
class ClassDef:
    name: str           # "Landroid/app/Activity;"
    access: int
    superclass: str | None
    interfaces: list[str]
    fields: dict[str, int]      # "name:type" -> access
    methods: dict[str, MethodDef]  # sig -> MethodDef
    origin: str

    @property
    def is_interface(self) -> bool:
        return bool(self.access & ACC_INTERFACE)

    @property
    def is_abstract(self) -> bool:
        return bool(self.access & (ACC_ABSTRACT | ACC_INTERFACE))


class Dex:
    def __init__(self, data: bytes, origin: str, with_code: bool):
        self.d = data
        self.origin = origin
        if data[:3] != b'dex':
            raise ValueError(f'{origin}: not a dex file')
        (self.string_ids_size, self.string_ids_off, self.type_ids_size, self.type_ids_off,
         self.proto_ids_size, self.proto_ids_off, self.field_ids_size, self.field_ids_off,
         self.method_ids_size, self.method_ids_off, self.class_defs_size,
         self.class_defs_off) = struct.unpack_from('<12I', data, 56)
        self._strings: dict[int, str] = {}
        self._protos: dict[int, str] = {}
        self.with_code = with_code

    def string(self, idx: int) -> str:
        s = self._strings.get(idx)
        if s is None:
            off = struct.unpack_from('<I', self.d, self.string_ids_off + idx * 4)[0]
            _, off = _uleb(self.d, off)
            end = self.d.index(b'\0', off)
            s = self.d[off:end].decode('utf-8', errors='surrogateescape')
            self._strings[idx] = s
        return s

    def type(self, idx: int) -> str:
        return self.string(struct.unpack_from('<I', self.d, self.type_ids_off + idx * 4)[0])

    def type_list(self, off: int) -> list[str]:
        if off == 0:
            return []
        n = struct.unpack_from('<I', self.d, off)[0]
        return [self.type(struct.unpack_from('<H', self.d, off + 4 + i * 2)[0]) for i in range(n)]

    def proto(self, idx: int) -> str:
        p = self._protos.get(idx)
        if p is None:
            _, ret, params_off = struct.unpack_from('<3I', self.d, self.proto_ids_off + idx * 12)
            p = '(' + ''.join(self.type_list(params_off)) + ')' + self.type(ret)
            self._protos[idx] = p
        return p

    def field_ref(self, idx: int) -> tuple[str, str]:
        cls, typ, name = struct.unpack_from('<HHI', self.d, self.field_ids_off + idx * 8)
        return self.type(cls), self.string(name) + ':' + self.type(typ)

    def method_ref(self, idx: int) -> tuple[str, str]:
        cls, proto, name = struct.unpack_from('<HHI', self.d, self.method_ids_off + idx * 8)
        return self.type(cls), self.string(name) + self.proto(proto)

    def _code_refs(self, code_off: int) -> list[tuple[str, tuple]]:
        d = self.d
        insns_size = struct.unpack_from('<I', d, code_off + 12)[0]
        pos = code_off + 16
        end = pos + insns_size * 2
        refs: list[tuple[str, tuple]] = []
        while pos < end:
            unit = d[pos] | (d[pos + 1] << 8)
            op = unit & 0xFF
            if op == 0x00 and unit != 0:            # payload pseudo-instructions
                if unit == 0x0100:
                    size = struct.unpack_from('<H', d, pos + 2)[0]
                    pos += (4 + size * 2) * 2
                elif unit == 0x0200:
                    size = struct.unpack_from('<H', d, pos + 2)[0]
                    pos += (2 + size * 4) * 2
                elif unit == 0x0300:
                    width = struct.unpack_from('<H', d, pos + 2)[0]
                    size = struct.unpack_from('<I', d, pos + 4)[0]
                    if width == 4:      # int arrays: styleable attr id tables
                        for v in struct.unpack_from(f'<{size}I', d, pos + 8):
                            if v >> 24 in RES_PACKAGES:
                                refs.append(('const', (v,)))
                    pos += (4 + (size * width + 1) // 2) * 2
                else:
                    pos += 2
                continue
            width, kind = _OPS[op]
            if kind == 'method':
                refs.append(('method', self.method_ref(struct.unpack_from('<H', d, pos + 2)[0])))
            elif kind == 'field':
                refs.append(('field', self.field_ref(struct.unpack_from('<H', d, pos + 2)[0])))
            elif kind == 'type':
                refs.append(('type', (self.type(struct.unpack_from('<H', d, pos + 2)[0]),)))
            elif op == 0x14:                        # const vAA, #+BBBBBBBB
                v = struct.unpack_from('<I', d, pos + 2)[0]
                if v >> 24 in RES_PACKAGES:                 # framework package (android:) resource id
                    refs.append(('const', (v,)))
            elif op == 0x15:                        # const/high16 vAA, #+BBBB0000
                v = struct.unpack_from('<H', d, pos + 2)[0] << 16
                if v >> 24 in RES_PACKAGES:
                    refs.append(('const', (v,)))
            pos += width * 2
        return refs

    def classes(self):
        d = self.d
        for i in range(self.class_defs_size):
            (cls_idx, access, super_idx, ifaces_off, _src, _ann, data_off,
             _sv) = struct.unpack_from('<8I', d, self.class_defs_off + i * 32)
            cd = ClassDef(
                name=self.type(cls_idx), access=access,
                superclass=None if super_idx == NO_INDEX else self.type(super_idx),
                interfaces=self.type_list(ifaces_off), fields={}, methods={},
                origin=self.origin)
            if data_off:
                off = data_off
                sf, off = _uleb(d, off)
                inf, off = _uleb(d, off)
                dm, off = _uleb(d, off)
                vm, off = _uleb(d, off)
                for count in (sf, inf):
                    idx = 0
                    for _ in range(count):
                        diff, off = _uleb(d, off)
                        acc, off = _uleb(d, off)
                        idx += diff
                        cd.fields[self.field_ref(idx)[1]] = acc
                for count in (dm, vm):
                    idx = 0
                    for _ in range(count):
                        diff, off = _uleb(d, off)
                        acc, off = _uleb(d, off)
                        code_off, off = _uleb(d, off)
                        idx += diff
                        _, sig = self.method_ref(idx)
                        name, _, rest = sig.partition('(')
                        m = MethodDef(name=name, proto='(' + rest, access=acc)
                        if self.with_code and code_off:
                            m.refs = self._code_refs(code_off)
                        cd.methods[m.sig] = m
            yield cd


def dex_blobs(path: Path):
    """Yield (origin, bytes) for every classes*.dex inside a jar/apk, or a bare .dex."""
    if path.suffix == '.dex':
        yield str(path), path.read_bytes()
        return
    try:
        with zipfile.ZipFile(path) as z:
            names = sorted((n for n in z.namelist() if n.startswith('classes') and n.endswith('.dex')),
                           key=lambda n: (len(n), n))
            for n in names:
                yield f'{path.name}!{n}', z.read(n)
    except zipfile.BadZipFile:
        return


def load_classes(paths, with_code: bool) -> dict[str, ClassDef]:
    """Load classes from several containers. First definition wins (classpath order)."""
    out: dict[str, ClassDef] = {}
    for p in paths:
        for origin, blob in dex_blobs(Path(p)):
            for cd in Dex(blob, origin, with_code).classes():
                out.setdefault(cd.name, cd)
    return out
