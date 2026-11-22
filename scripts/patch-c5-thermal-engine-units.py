#!/usr/bin/env python3
"""HOST-only conversion of the exact C5 thermal-engine TSENS sysfs client."""
import argparse
import hashlib
from pathlib import Path

ORIGINAL_SHA256 = '91fbcaffc7c80646eeb2a31b96b4c937135cc6c534da399746451b22528c4c71'
FIXED_SHA256 = 'cab5e312876f4a6bd7635f47b48212d8dfe765d906ed93af8199b2a7cd7bcec2'
EDITS = (
    (0xb184, bytes.fromhex('147c081b'), bytes.fromhex('f403002a')),
    (0xb2e0, bytes.fromhex('087d099b'), bytes.fromhex('05000014')),
    (0xb300, bytes.fromhex('087d099b'), bytes.fromhex('05000014')),
)


def transform(raw):
    digest = hashlib.sha256(raw).hexdigest()
    if digest == FIXED_SHA256:
        return raw
    if digest != ORIGINAL_SHA256:
        raise ValueError('Exact original C5 thermal-engine required; no output')
    result = bytearray(raw)
    for offset, old, new in EDITS:
        if raw[offset:offset + len(old)] != old:
            raise ValueError('Unexpected original TSENS instruction')
        result[offset:offset + len(old)] = new
    if hashlib.sha256(result).hexdigest() != FIXED_SHA256:
        raise ValueError('Unexpected thermal-engine output')
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.input.is_symlink() or args.output.exists() or args.output.is_symlink():
        raise ValueError('Regular input and newly owned output required')
    result = transform(args.input.read_bytes())
    with args.output.open('xb') as out:
        out.write(result)


if __name__ == '__main__':
    main()
