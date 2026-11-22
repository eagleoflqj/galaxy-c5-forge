#!/usr/bin/env python3
"""HOST-only exact-input C5 normal-system configfs/FunctionFS USB overlay."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path

PINS = {
    'system/core/rootdir/init.usb.rc': '4dde3aa1f087c79abdc960af50897a60ae794e742c6f1086e1c7a3d252eee2c6',
    'system/core/rootdir/init.usb.configfs.rc': '1384154db54fe5a8b3fb62f9c8e9c6732c379066c941364c25d853ac55aa994b',
    'device/samsung/c5lte/rootdir/etc/init.qcom.usb.rc': '0e7c56bb92a4842ba2332c5b39e3993c879704109e163e0e530c6bf41bc8de9b',
}
MODES = ('adb', 'mtp', 'mtp,adb', 'ptp', 'ptp,adb', 'rndis', 'rndis,adb')
LEGACY = '/sys/class/android_usb/android0/'
BASE = '/config/usb_gadget/g1'
sha = lambda data: hashlib.sha256(data).hexdigest()


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sections(text):
    starts = [m.start() for m in re.finditer(r'^(?:on |service )', text, re.M)]
    require(starts, 'No init sections')
    return text[:starts[0]], [text[a:b].rstrip() + '\n' for a, b in zip(starts, starts[1:] + [len(text)])]


def action(blocks, header):
    found = [b for b in blocks if b.splitlines()[0] == header]
    require(len(found) == 1, 'Missing/ambiguous action: ' + header)
    return found[0]


def ids(usb, generic):
    _, vendor = sections(usb)
    _, core = sections(generic)
    result = {}
    for mode in MODES:
        oldmode = {'rndis': 'rndis,none', 'rndis,adb': 'rndis,none,adb'}.get(mode, mode)
        block = action(core, 'on property:sys.usb.config=adb && property:sys.usb.configfs=0') if mode == 'adb' else action(vendor, 'on property:sys.usb.config=' + oldmode)
        pair = []
        for name in ('idVendor', 'idProduct'):
            values = re.findall(r'^    write ' + re.escape(LEGACY + name) + r' ([0-9a-fA-F]+)$', block, re.M)
            require(len(values) == 1 and 0 < int(values[0], 16) < 65536, 'Unreviewed USB ID')
            pair.append('0x' + values[0].lower())
        result[mode] = pair
    return result


def transform(generic, configfs, vendor):
    identifiers = ids(vendor, generic)
    prefix, blocks = sections(generic)
    kept = []
    removed = []
    for block in blocks:
        header = block.splitlines()[0]
        if 'property:sys.usb.configfs=0' in header:
            require(LEGACY in block, 'Unreviewed legacy composition')
            removed.append(header)
            continue
        if header == 'on post-fs-data':
            lines = block.splitlines()
            legacy = [line for line in lines if LEGACY in line]
            require(len(legacy) == 4 and all(line.strip().startswith(('chown ', 'chmod ')) for line in legacy), 'Unexpected legacy permissions')
            block = '\n'.join(line for line in lines if LEGACY not in line) + '\n'
        if header == 'on boot':
            require(block.count('setprop sys.usb.configfs 0') == 1, 'Unknown core configfs startup')
            block = block.replace('setprop sys.usb.configfs 0', 'setprop sys.usb.configfs 1')
        kept.append(block)
    normal = prefix + '\n'.join(kept)
    require(LEGACY not in normal, 'Legacy composition survived')
    # Preserve the real adbd service/authentication and existing persistent
    # USB selection logic;this overlay does not force root or bypass keys.
    require(action(sections(normal)[1], 'service adbd /system/bin/adbd --root_seclabel=u:r:su:s0') == action(blocks, 'service adbd /system/bin/adbd --root_seclabel=u:r:su:s0'), 'adbd service changed')
    require(action(sections(normal)[1], 'on boot && property:persist.sys.usb.config=*') == action(blocks, 'on boot && property:persist.sys.usb.config=*'), 'Persistent selection changed')
    cfg_prefix, cfg_blocks = sections(configfs)
    supported = []
    for block in cfg_blocks:
        header = block.splitlines()[0]
        if header == 'on property:init.svc.adbd=stopped':
            supported.append(block)
            continue
        match = re.search(r'property:sys.usb.config=([^ ]+)', header)
        require(match is not None, 'Unknown platform configfs trigger')
        if match[1] in ('none', *MODES):
            require('property:sys.usb.configfs=1' in header, 'Unguarded configfs action')
            supported.append(block)
    native = cfg_prefix + '\n'.join(supported)
    adb_id = identifiers['adb']
    setup = ['# C5 native4.9 normal-system USB stage1;not a recovery/authentication bypass.',
             '# Supported: adb,mtp,ptp,rndis and their adb combinations.',
             '# Unsupported factory/diagnostic/accessory/audio/MIDI compositions stay OPEN.',
             'on early-init', '    setprop ro.adb.nonblocking_ffs 1',
             '    setprop sys.usb.ffs.aio_compat 0', '', 'on init',
             '    mkdir ' + BASE + ' 0755 root root',
             '    write ' + BASE + '/idVendor ' + adb_id[0],
             '    write ' + BASE + '/idProduct ' + adb_id[1],
             '    mkdir ' + BASE + '/strings/0x409 0755 root root',
             '    write ' + BASE + '/strings/0x409/serialnumber ${ro.serialno}',
             '    write ' + BASE + '/strings/0x409/manufacturer ${ro.product.manufacturer}',
             '    write ' + BASE + '/strings/0x409/product ${ro.product.model}',
             '    mkdir ' + BASE + '/configs/b.1 0755 root root',
             '    mkdir ' + BASE + '/configs/b.1/strings/0x409 0755 root root',
             '    mkdir ' + BASE + '/functions/ffs.adb 0755 root root',
             '    mkdir ' + BASE + '/functions/mtp.gs0 0755 root root',
             '    mkdir ' + BASE + '/functions/ptp.gs1 0755 root root',
             '    mkdir /dev/usb-ffs 0770 shell shell',
             '    mkdir /dev/usb-ffs/adb 0770 shell shell',
             '    mount functionfs adb /dev/usb-ffs/adb uid=2000,gid=2000',
             '    setprop sys.usb.controller msm_hsusb', '']
    for mode, (vid, pid) in identifiers.items():
        setup += ['on property:sys.usb.config=' + mode + ' && property:sys.usb.configfs=1',
                  '    write ' + BASE + '/idVendor ' + vid, '    write ' + BASE + '/idProduct ' + pid, '']
    device = '\n'.join(setup)
    require(not any(token in device for token in ('service.adb.root', 'ro.adb.secure', 'persist.', 'setenforce', '/data/', '/dev/block/')), 'Device overlay expands state/permission scope')
    return {'init.usb.rc': normal.encode(), 'init.usb.configfs.rc': native.encode(),
            'vendor/etc/init/hw/init.qcom.usb.rc': device.encode()}, identifiers, removed


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--android', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    a = p.parse_args()
    require(a.android.is_absolute() and a.output.is_absolute() and not a.output.exists() and not a.output.is_symlink(), 'Fresh absolute output required')
    require(not a.output.resolve().is_relative_to(a.android.resolve()), 'Output overlaps accepted source')
    values = {}
    for name, pin in PINS.items():
        raw = (a.android / name).read_bytes()
        require(sha(raw) == pin, 'Unreviewed input: ' + name)
        values[name] = raw.decode()
    files, identifiers, removed = transform(*(values[name] for name in PINS))
    os.umask(0o077)
    a.output.mkdir(mode=0o700)
    for name, data in files.items():
        path = a.output / name
        path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        path.write_bytes(data)
    report = dict(result='HOST_NATIVE_USB_OVERLAY_PREPARED_NOT_INSTALLED_OR_HARDWARE_ACCEPTED',
                  inputs=PINS, script_sha256=sha(Path(__file__).read_bytes()), outputs={name: sha(data) for name, data in files.items()},
                  source_derived_mode_identifiers=identifiers, removed_legacy_compositions=removed,
                  normal_adbd_service_and_auth_and_persistent_selection='UNCHANGED',
                  device_commands=[], limitations='Seven normal modes only;no factory/diagnostic/accessory/audio/MIDI acceptance. Platform FunctionFS-ready/none/stop/link/bind/state actions copied unchanged for supported modes,not an actual USB enumeration or reconnect test. Preserve actual init import ordering,configfs mount,SELinux and current persistent USB choice before a later authorized ordinary-system boot. Payload belongs in system-side init,not accepted BOOT RC;no image or device mutation is performed.')
    (a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
