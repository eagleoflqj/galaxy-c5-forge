#!/usr/bin/env python3
"""Prepare guarded legacy/current USB profiles for a controlled kernel comparison."""
import hashlib
import re

PINS = {
    'current_root': '09f9a91e79acd80c5e1138cf995b1fb540d22341b8c5d12c5a9bfafba361b470',
    'current_vendor': 'f3ba9dbdd727d2bdc69d4af0df333681cb310e0c6f2c0bfbc1257f8c155ed190',
    'legacy_root': '4dde3aa1f087c79abdc960af50897a60ae794e742c6f1086e1c7a3d252eee2c6',
    'legacy_vendor': '0e7c56bb92a4842ba2332c5b39e3993c879704109e163e0e530c6bf41bc8de9b',
}
PROFILE = '${ro.boot.c5_usb_configfs:-1}'
ACCEPTED_BOOT_SHA = 'f55f800f85eb91f47594f54548b722bfa6dd56c087b3e319a9406dfec9ebf612'


def legacy_control_boot(raw):
    """Change only the known accepted short BOOT's first cmdline field."""
    if hashlib.sha256(raw).hexdigest() != ACCEPTED_BOOT_SHA:
        raise ValueError('Exact accepted 3.10 short BOOT required')
    if len(raw) < 2048 or raw[:8] != b'ANDROID!' or any(raw[608:1632]):
        raise ValueError('Unexpected accepted Samsung cmdline layout')
    old = raw[64:576].split(b'\0', 1)[0]
    if b'c5_usb_configfs' in old:
        raise ValueError('USB profile already present')
    cmdline = old + b' androidboot.c5_usb_configfs=0'
    if len(cmdline) > 511:
        raise ValueError('Do not depend on extra-cmdline bootloader support')
    result = bytearray(raw)
    result[64:576] = cmdline.ljust(512, b'\0')
    return bytes(result)


def actions(raw):
    """Keep exact action text; reject service/import sections in selected inputs."""
    result = []
    header = None
    body = []
    for line in raw.splitlines(True):
        if line.startswith(('on ', 'service ', 'import ')):
            if header is not None:
                result.append((header, ''.join(body)))
            header, body = line.rstrip('\n'), []
        elif header is not None:
            body.append(line)
    if header is not None:
        result.append((header, ''.join(body)))
    return result


def transform(inputs):
    if inputs.keys() != PINS.keys() or any(hashlib.sha256(inputs[k]).hexdigest() != h for k, h in PINS.items()):
        raise ValueError('Exact accepted/current USB sources required')
    root = inputs['current_root'].decode()
    vendor = inputs['current_vendor'].decode()
    legacy_root = inputs['legacy_root'].decode()
    legacy_vendor = inputs['legacy_vendor'].decode()
    if root.count('    setprop sys.usb.configfs 1\n') != 1 or vendor.count('    setprop ro.adb.nonblocking_ffs 1\n') != 1 or vendor.count('\non init\n') != 1:
        raise ValueError('Ambiguous current USB profile')
    root = root.replace('    setprop sys.usb.configfs 1\n', '    setprop sys.usb.configfs '+PROFILE+'\n')
    vendor = vendor.replace('    setprop ro.adb.nonblocking_ffs 1\n', '    setprop ro.adb.nonblocking_ffs '+PROFILE+'\n    setprop sys.usb.configfs '+PROFILE+'\n')
    vendor = vendor.replace('\non init\n', '\non init && property:sys.usb.configfs=1\n')
    selected_root = []
    for head, body in actions(legacy_root):
        if head in ('on property:sys.usb.config=none && property:sys.usb.configfs=0', 'on property:sys.usb.config=adb && property:sys.usb.configfs=0'):
            selected_root.append(head+'\n'+body)
    if len(selected_root) != 2:
        raise ValueError('Missing accepted legacy adb/none actions')
    root += '\n# Accepted legacy gadget, selected only by the control BOOT property.\n' + '\n'.join(selected_root)
    selected_vendor = []
    modes = {'mtp', 'mtp,adb', 'ptp', 'ptp,adb'}
    for head, body in actions(legacy_vendor):
        mode = re.fullmatch(r'on property:sys.usb.config=(.*)', head)
        if head in ('on boot', 'on fs'):
            # Exclude unrelated RNDIS setup and the old persistent-extra reset.
            body = ''.join(line for line in body.splitlines(True) if '/f_rndis/' not in line and 'setprop persist.' not in line)
        elif not mode or mode[1] not in modes:
            continue
        selected_vendor.append(head+' && property:sys.usb.configfs=0\n'+body)
    if len(selected_vendor) != 6:
        raise ValueError('Missing accepted legacy startup/mtp/ptp actions')
    license_header = legacy_vendor[:legacy_vendor.index('on early-init')]
    vendor += '\n# Legacy source notice and scoped accepted gadget actions.\n'+license_header+'\n'.join(selected_vendor)
    if 'setprop persist.' in vendor or 'functions diag' in vendor:
        raise ValueError('Unexpected persistent setting or diagnostic composition')
    return {'root': root.encode(), 'vendor': vendor.encode()}
