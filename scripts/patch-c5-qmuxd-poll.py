#!/usr/bin/env python3
"""HOST-only C5 QMUX poll compatibility; preserve data/reset handling and watchdogs.

The stock daemon reads after every nonnegative poll return. Its original kernel
does not emit POLLPRI, whereas the native 4.9 SMD packet driver does. Stop asking
for priority-only events, which carry no packet for that following blocking read.
Proprietary input/output must remain outside this repository.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

BEFORE_SHA = '511a041b34ce88021aa85aabd7b2e07b4a01f148cbb104772b8b8ec81dccb610'
SITE = 0xbca0
OLD = 0x52800169  # mov w9, #11: POLLIN | POLLPRI | POLLERR
NEW = 0x52800129  # mov w9, #9:  POLLIN | POLLERR
GUARDS = {
    SITE: OLD,
    0xbca4: 0x79007be9,  # store events in the original pollfd
    0xbd8c: 0x97ffe971,  # existing __poll_chk call
    0xbd90: 0x2a0003fa,  # preserve poll return
    0xbd94: 0x37f81eda,  # existing negative-return/error branch
    0xbda0: 0x94000297,  # existing watchdog start
    0xbfd0: 0x97ffe874,  # existing blocking read
    0xc8dc: 0x528000a8,  # existing five-second watchdog interval
    0xcaf0: 0x97ffe630,  # existing abort, never patched out
}


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def replace_verified_word(raw):
    for offset, word in GUARDS.items():
        if len(raw) < offset + 4 or struct.unpack_from('<I', raw, offset)[0] != word:
            raise ValueError('Exact stock poll/read/watchdog instructions required')
    result = bytearray(raw)
    struct.pack_into('<I', result, SITE, NEW)
    if {i for i, (a, b) in enumerate(zip(raw, result)) if a != b} != {SITE}:
        raise ValueError('Only the priority-event immediate bit may change')
    return bytes(result)


def transform(raw):
    if sha(raw) != BEFORE_SHA or raw[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', raw, 18)[0] != 183:
        raise ValueError('Only the exact original C5 AArch64 QMUX daemon is supported')
    return replace_verified_word(raw)


def review(before, after):
    if after != transform(before):
        raise ValueError('Exactly one poll-mask immediate change is allowed')
    return dict(before_sha256=sha(before), after_sha256=sha(after), bytes=len(after),
                changed_instruction_offset=SITE, old_events=11, new_events=9,
                poll_priority_only_events_removed=True, data_and_reset_errors_retained=True,
                watchdog_interval_and_abort_byte_identical=True,
                read_write_ioctl_and_permissions_byte_identical=True,
                live_QMUX_stability_or_GNSS_PASS=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    if not args.input.is_absolute() or not args.output.is_absolute() or args.input.is_symlink() or args.output.exists() or args.output.is_symlink() or args.output.resolve().is_relative_to(root):
        parser.error('Absolute input and fresh output outside the repository required')
    raw = args.input.read_bytes()
    after = transform(raw)
    with args.output.open('xb') as stream:
        stream.write(after)
    print(json.dumps(review(raw, after), indent=2))


if __name__ == '__main__':
    main()
