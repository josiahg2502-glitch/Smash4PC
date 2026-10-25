#!/usr/bin/env python3
"""Read-only RPX/RPL inventory; no relocation, execution, or recompilation.

ELF32 structures plus Cafe section compression (SHF_RPL_ZLIB). Format reference:
https://github.com/Maschell/GhidraRPXLoader/blob/master/src/main/java/cafeloader/RplConverter.java
"""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import zlib


def inspect(path):
    raw = Path(path).read_bytes()
    if len(raw) < 52 or raw[:7] != b"\x7fELF\x01\x02\x01":
        raise ValueError("Expected a big-endian ELF32 RPX/RPL")
    hdr = struct.unpack_from(">HHIIIIIHHHHHH", raw, 16)
    elf_type, machine, version, entry, _, shoff, _, ehsize, _, _, shentsize, shnum, shstrndx = hdr
    if machine != 20 or version != 1 or ehsize != 52 or shentsize != 40:
        raise ValueError("Unsupported ELF32 PowerPC header")
    if not shnum or shstrndx >= shnum or shoff + shnum * shentsize > len(raw):
        raise ValueError("Invalid section header table")
    sections, payloads = [], []
    for index in range(shnum):
        fields = struct.unpack_from(">10I", raw, shoff + index * shentsize)
        nameoff, kind, flags, addr, offset, size, link, info, alignment, entsize = fields
        payload = b""
        if kind != 8 and size:
            if offset + size > len(raw):
                raise ValueError(f"Section {index} exceeds input")
            payload = raw[offset:offset + size]
            if flags & 0x08000000:
                if size < 4:
                    raise ValueError(f"Section {index} has truncated zlib prefix")
                expected, = struct.unpack_from(">I", payload)
                if expected > 512 * 1024 * 1024:
                    raise ValueError(f"Section {index} exceeds analysis size limit")
                stream = zlib.decompressobj()
                payload = stream.decompress(payload[4:], expected + 1)
                if len(payload) != expected or not stream.eof or stream.unused_data:
                    raise ValueError(f"Section {index} has invalid compressed size or stream")
        sections.append(dict(index=index, name_offset=nameoff, type=f"0x{kind:08x}",
                             flags=f"0x{flags:08x}", address=f"0x{addr:08x}",
                             file_offset=offset, stored_size=size,
                             size=size if kind == 8 else len(payload),
                             link=link, info=info, alignment=alignment, entry_size=entsize))
        payloads.append(payload)

    def cstring(data, offset):
        if offset >= len(data):
            raise ValueError(f"String offset {offset} exceeds string table")
        end = data.find(b"\0", offset)
        if end < 0:
            raise ValueError("Unterminated string")
        return data[offset:end].decode("utf-8", errors="replace")

    for section in sections:
        section["name"] = cstring(payloads[shstrndx], section.pop("name_offset"))
    imports, functions = [], []
    symbol_count = 0
    relocations = Counter()
    for section, payload in zip(sections, payloads):
        kind = int(section["type"], 16)
        if kind in (2, 11):  # SHT_SYMTAB / SHT_DYNSYM
            if section["entry_size"] != 16 or len(payload) % 16 or section["link"] >= shnum:
                raise ValueError("Invalid symbol table")
            names = payloads[section["link"]]
            for pos in range(0, len(payload), 16):
                nameoff, value, size, info, other, shndx = struct.unpack_from(">IIIBBH", payload, pos)
                name = cstring(names, nameoff)
                symbol_count += 1
                if shndx >= shnum:
                    continue
                target = sections[shndx]
                record = dict(name=name, address=f"0x{value:08x}", size=size,
                              symbol_type=info & 15, binding=info >> 4,
                              section=target["name"], symbol_index=pos // 16)
                if int(target["type"], 16) == 0x80000002 and name and info & 15 in (1, 2):
                    record["module"] = target["name"].split("import_", 1)[-1]
                    record["kind"] = "function" if target["name"].startswith(".fimport_") else "data"
                    imports.append(record)
                elif info & 15 == 2 and shndx and name:
                    functions.append(record)
        elif kind in (4, 9):  # SHT_RELA / SHT_REL
            stride = 12 if kind == 4 else 8
            if section["entry_size"] != stride or len(payload) % stride:
                raise ValueError("Invalid relocation table")
            for pos in range(0, len(payload), stride):
                _, info = struct.unpack_from(">II", payload, pos)
                relocations[str(info & 255)] += 1
    code_sections = [s for s in sections if int(s["type"], 16) == 1 and int(s["flags"], 16) & 4]
    return dict(path=str(Path(path)), sha256=hashlib.sha256(raw).hexdigest(),
                file_size=len(raw), elf_type=f"0x{elf_type:04x}", architecture="PowerPC 32-bit big-endian",
                entry_point=f"0x{entry:08x}", sections=sections, symbol_count=symbol_count,
                named_defined_function_count=len(functions), defined_functions=functions,
                import_count=len(imports), imports=imports,
                imports_by_module=dict(sorted(Counter(i["module"] for i in imports).items())),
                relocation_count=sum(relocations.values()), relocation_types=dict(sorted(relocations.items())),
                executable_section_bytes=sum(s["size"] for s in code_sections),
                notes=["Inventory only: relocations have not been applied.",
                       "Static imports omit libraries/functions acquired dynamically at runtime.",
                       "Named function symbols are not a complete recovered function inventory."])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rpx", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        result = inspect(args.rpx)
    except (OSError, ValueError, struct.error, zlib.error) as error:
        parser.exit(1, f"rpx_inspect: {error}\n")
    output = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output)
        print(f"{args.rpx}: entry {result['entry_point']}, {result['import_count']} imports, "
              f"{result['executable_section_bytes']} code bytes; wrote {args.output}")
    else:
        print(output, end="")


if __name__ == "__main__":
    main()
