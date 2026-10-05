#!/usr/bin/env python3
"""Replace same-size-or-shorter strings in an ELF .dynstr section in place."""

import argparse
import struct
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    parser.add_argument("replacement", nargs="+", metavar="OLD=NEW")
    return parser.parse_args()


def section_headers(data):
    if data[:4] != b"\x7fELF":
        raise ValueError("input is not ELF")
    elf_class = data[4]
    elf_data = data[5]
    if elf_data != 1:
        raise ValueError("only little-endian ELF files are supported")

    if elf_class == 1:
        ehdr = struct.unpack_from("<16sHHIIIIIHHHHHH", data, 0)
        shoff, shentsize, shnum, shstrndx = ehdr[6], ehdr[11], ehdr[12], ehdr[13]
        shdr_format = "<IIIIIIIIII"
        offset_index, size_index = 4, 5
    elif elf_class == 2:
        ehdr = struct.unpack_from("<16sHHIQQQIHHHHHH", data, 0)
        shoff, shentsize, shnum, shstrndx = ehdr[6], ehdr[11], ehdr[12], ehdr[13]
        shdr_format = "<IIQQQQIIQQ"
        offset_index, size_index = 4, 5
    else:
        raise ValueError("unsupported ELF class")

    expected_shentsize = struct.calcsize(shdr_format)
    if shentsize != expected_shentsize or shnum == 0 or shstrndx >= shnum:
        raise ValueError("unsupported or invalid ELF section table")
    if shoff + shentsize * shnum > len(data):
        raise ValueError("ELF section table extends beyond the file")

    headers = []
    for index in range(shnum):
        header = struct.unpack_from(shdr_format, data, shoff + index * shentsize)
        headers.append((header[0], header[1], header[offset_index], header[size_index]))
    return headers, shstrndx


def section_name(names, offset):
    if offset >= len(names):
        raise ValueError("invalid section-name offset")
    end = names.find(b"\0", offset)
    if end < 0:
        raise ValueError("unterminated section name")
    return names[offset:end]


def dynstr_bounds(data):
    headers, shstrndx = section_headers(data)
    _, _, names_offset, names_size = headers[shstrndx]
    if names_offset + names_size > len(data):
        raise ValueError("section-name table extends beyond the file")
    names = data[names_offset:names_offset + names_size]
    matches = []
    for name_offset, section_type, offset, size in headers:
        if section_name(names, name_offset) == b".dynstr":
            if section_type != 3 or offset + size > len(data):
                raise ValueError("invalid .dynstr section")
            matches.append((offset, size))
    if len(matches) != 1:
        raise ValueError(f"expected one .dynstr section, found {len(matches)}")
    return matches[0]


def parse_replacement(value):
    old, separator, new = value.partition("=")
    if not separator or not old or not new:
        raise ValueError(f"invalid replacement: {value}")
    old_bytes = old.encode("ascii")
    new_bytes = new.encode("ascii")
    if len(new_bytes) > len(old_bytes):
        raise ValueError(f"replacement is longer than source: {value}")
    return old_bytes, new_bytes


def main():
    args = parse_args()
    data = bytearray(args.elf.read_bytes())
    offset, size = dynstr_bounds(data)
    dynstr = data[offset:offset + size]

    reports = []
    for value in args.replacement:
        old, new = parse_replacement(value)
        needle = old + b"\0"
        matches = []
        cursor = 0
        while True:
            found = dynstr.find(needle, cursor)
            if found < 0:
                break
            if found == 0 or dynstr[found - 1] == 0:
                matches.append(found)
            cursor = found + len(needle)
        if len(matches) != 1:
            raise ValueError(f"expected one exact .dynstr entry for {old.decode()}, found {len(matches)}")
        replacement = new + b"\0" * (len(old) - len(new) + 1)
        start = matches[0]
        dynstr[start:start + len(needle)] = replacement
        reports.append((old.decode(), new.decode()))

    data[offset:offset + size] = dynstr
    args.elf.write_bytes(data)
    for old, new in reports:
        print(f"PATCHED {args.elf} {old} {new}")


if __name__ == "__main__":
    main()
