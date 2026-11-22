#!/usr/bin/env python3
"""HOST-only exact C5 AArch64 time-client MSG_NOSIGNAL correction.

Change send flags, not return values, peer authorization, time or authentication.
Proprietary input/output stay outside the orchestration repository.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

BEFORE_SHA = 'cb27efb78a125cd9c19aee35fbdc76eec58eeaea8225e508e404ca6ea79e2e34'
SITES = ((0xadc, 0xb00), (0xbe0, 0xc00), (0xcf0, 0xd0c))
OLD = 0x2a1f03e3  # mov w3, wzr: send flags = 0
NEW = 0x52880003  # mov w3, #0x4000: send flags = MSG_NOSIGNAL
SEND_PLT = 0x838


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def replace_verified_words(raw):
    result=bytearray(raw)
    for flags, call in SITES:
        if len(raw)<call+4 or struct.unpack_from('<I',raw,flags)[0]!=OLD:
            raise ValueError('Exact send-flags instruction required')
        branch=struct.unpack_from('<I',raw,call)[0]
        immediate=branch & 0x3ffffff
        if immediate & 0x2000000:
            immediate-=0x4000000
        if branch>>26!=0b100101 or call+immediate*4!=SEND_PLT:
            raise ValueError('Exact existing send PLT call required')
        struct.pack_into('<I',result,flags,NEW)
    expected={offset+i for offset,_ in SITES for i in range(4) if struct.pack('<I',OLD)[i]!=struct.pack('<I',NEW)[i]}
    if {i for i,(a,b) in enumerate(zip(raw,result)) if a!=b}!=expected:
        raise ValueError('Unexpected binary delta')
    return bytes(result)


def transform(raw):
    if sha(raw)!=BEFORE_SHA or raw[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<H',raw,18)[0]!=183:
        raise ValueError('Only the exact verified C5 AArch64 time client is supported')
    return replace_verified_words(raw)


def review(before,after):
    if after!=transform(before):
        raise ValueError('Only the three MSG_NOSIGNAL flags changes are allowed')
    return dict(result='HOST_C5_TIME_CLIENT_NOSIGNAL_NOT_HARDWARE_ACCEPTANCE',before_sha256=sha(before),after_sha256=sha(after),bytes=len(after),changed_instruction_offsets=[a for a,_ in SITES],send_error_returns_unchanged=True,peer_authorization_unchanged=True,authentication_unchanged=True,hardware_accepted=False)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--input',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    if not args.input.is_absolute() or not args.output.is_absolute() or args.input.is_symlink() or args.output.exists() or args.output.is_symlink() or args.output.resolve().is_relative_to(root):
        parser.error('Absolute non-symlink input and fresh output outside repository required')
    raw=args.input.read_bytes();after=transform(raw)
    with args.output.open('xb') as stream:
        stream.write(after)
    print(json.dumps(review(raw,after),indent=2))


if __name__=='__main__':
    main()
