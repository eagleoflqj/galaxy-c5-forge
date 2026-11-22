#!/usr/bin/env python3
"""Package an unverified C5 4.9 recovery; never access a phone or flash it."""
import argparse
import gzip
import hashlib
import importlib.util
import json
import os
import re
import struct
import subprocess
from pathlib import Path, PurePosixPath

_memory_spec = importlib.util.spec_from_file_location('c5_memory', Path(__file__).with_name('kernel-c5-memory-audit.py'))
memory_contract = importlib.util.module_from_spec(_memory_spec)
_memory_spec.loader.exec_module(memory_contract)

RESCUE_SHA = '2ab7a2e63d86176b11a333cddcbc020ebb31fb897ffa8af31fd94d6e9c4b3fb9'
MKBOOTIMG_SHA = '941a8dae9986861b86e72a188c100321f5258b5967a1f065a4a19d50dae3e44a'
DTBTOOL_SHA = '6d7af550f246504cbef27d052764cb1681e09872f5add5667e9231ca3189fdab'
DTB_SHA = '396f104f2e9ad30ed83cca198cdec4eaec8def965c2712d1a2abf6a3f0c45ad8'
DTC_SHA = 'd386e2ba413335cdd633f0da1344f19554d74e7f9418662de06bde2a6b2fa434'
SOURCE_LINKS_SHA = '2fc2794ae817c7108e2c3300557d54db5b487313686aa19b245019b4a0a10d65'
SOURCE_INVENTORY_SHA = '0799d76d79ccdbb4d758ebf3dc0e62cd30d797dbea5f2f5fb1c043a81c364f9b'
CONFIG_SHA = 'd054793b147b542591355c4b129955e3226ebeaa617d276e1f014f3cd2cf4fd7'
REQUIRED = '''ARCH_MSM8952 ARM_PSCI_FW CPU_FREQ_MSM CPU_FREQ_GOV_INTERACTIVE
MSM_PIL MSM_PIL_SSR_GENERIC MSM_PIL_MSS_QDSP6V5 MSM_SUBSYSTEM_RESTART
MSM_SPM MSM_SMP2P MAILBOX KEYBOARD_GPIO MMC_SDHCI_MSM EXT4_FS F2FS_FS
BATTERY_SAMSUNG BATTERY_SAMSUNG_V2 BATTERY_SWELLING BATTERY_AGE_FORECAST
MULTI_CHARGING STEP_CHARGING FUELGAUGE_SM5705 PREVENT_SOC_JUMP CHARGER_SM5705
CHARGER_DA9155 MFD_SM5705 REGULATOR_SM5705 SEC_MPP_SHARE SAMSUNG_JACK
SEC_MPP_SHARE_LOW_TEMP_WA MUIC_NOTIFIER MUIC_UNIVERSAL MUIC_UNIVERSAL_SM5705
MUIC_UNIVERSAL_SM5705_AFC VBUS_NOTIFIER C5_RECOVERY_ONLY THERMAL
THERMAL_TSENS8974 THERMAL_MONITOR THERMAL_QPNP THERMAL_QPNP_ADC_TM
USB_MSM_OTG USB_CONFIGFS USB_CONFIGFS_F_FS USB_CONFIGFS_F_MTP
USB_CONFIGFS_UEVENT AIO SECURITY_SELINUX SECCOMP SECCOMP_FILTER
TOUCHSCREEN_ZT7538 FB_MSM_MDSS_SAMSUNG PANEL_S6E3FA3_AMS520KG09_FHD
C5LTE_SMART_DIMMING'''.split()


def validate_memory_contract(dtb, config_text):
    report = memory_contract.audit(memory_contract.board.tree(dtb), config_text,
                                   memory_contract.board.layout(dtb)['memory_reservations'])
    require(not report['blockers'], 'Pre-flash memory/config review HOLD: ' + '; '.join(report['blockers']))
    return report


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def align(n, page=2048):
    return (n + page - 1) // page * page


def boot_parts(data):
    require(len(data) >= 2048 and data[:8] == b'ANDROID!', 'Bad boot header')
    fields = struct.unpack_from('<10I', data, 8)
    ks, ka, rs, ra, ss, sa, ta, page, ds, os_version = fields
    require((ka, ra, sa, ta, page, ss) == (0x80008000, 0x82000000, 0x80f00000, 0x81e00000, 2048, 0),
            'Not the measured C5 Samsung boot layout')
    kernel = data[page:page + ks]
    roff = page + align(ks)
    ramdisk = data[roff:roff + rs]
    doff = roff + align(rs)
    dt = data[doff:doff + ds]
    end = doff + align(ds)
    require(len(data) == end + 16 and data[end:] == b'SEANDROIDENFORCE', 'Bad extent/trailer')
    require(all((ks, rs, ds)), 'Missing kernel, ramdisk or DT image')
    cmdline = data[64:576].split(b'\0')[0] + data[608:1632].split(b'\0')[0]
    return {'fields': fields, 'cmdline': cmdline.decode(), 'kernel': kernel,
            'ramdisk': ramdisk, 'dt': dt, 'id': data[576:608]}


def cpio_read(data):
    entries, names, off = [], set(), 0
    while off + 110 <= len(data):
        header = data[off:off + 110]
        require(header[:6] == b'070701', 'Only Android newc CPIO is accepted')
        fields = [int(header[6 + i * 8:14 + i * 8], 16) for i in range(13)]
        off += 110
        size, namesize = fields[6], fields[11]
        require(namesize > 0 and off + namesize <= len(data), 'Truncated CPIO name')
        raw_name = data[off:off + namesize]
        require(raw_name[-1:] == b'\0' and b'\0' not in raw_name[:-1], 'Bad CPIO name terminator')
        name = raw_name[:-1].decode()
        path = PurePosixPath(name)
        require(not path.is_absolute() and '..' not in path.parts and name not in names,
                'Unsafe or duplicate CPIO name')
        names.add(name)
        off = align(off + namesize, 4)
        require(off + size <= len(data), 'Truncated CPIO content')
        payload = data[off:off + size]
        off = align(off + size, 4)
        entries.append((name, fields, payload))
        if name == 'TRAILER!!!':
            require(size == 0, 'Nonempty CPIO trailer')
            require(not any(data[off:]), 'Unexpected data after CPIO trailer')
            return entries
    raise ValueError('Missing CPIO trailer')


def cpio_write(entries):
    result = bytearray()
    for name, old_fields, payload in entries:
        fields = old_fields.copy()
        encoded = name.encode() + b'\0'
        fields[6], fields[11] = len(payload), len(encoded)
        result.extend(b'070701' + ''.join(f'{v:08x}' for v in fields).encode())
        result.extend(encoded)
        result.extend(b'\0' * (align(len(result), 4) - len(result)))
        result.extend(payload)
        result.extend(b'\0' * (align(len(result), 4) - len(result)))
    result.extend(b'\0' * (align(len(result), 512) - len(result)))
    return bytes(result)


def native_ramdisk(entries):
    changes, result = {}, []
    for name, fields, data in entries:
        after = data
        if name == 'init.rc':
            require(data.count(b'setprop sys.usb.configfs 0') == 1, 'Unknown recovery USB init')
            after = data.replace(b'setprop sys.usb.configfs 0', b'setprop sys.usb.configfs 1')
        elif name == 'init.recovery.qcom.rc':
            old = (b'    # Android 10 adbd defaults to nonblocking FunctionFS AIO, which this 3.10 kernel does not support.\n'
                   b'    setprop ro.adb.nonblocking_ffs 0\n'
                   b'    setprop sys.usb.ffs.aio_compat 1\n')
            require(data.count(old) == 1, 'Unknown accepted legacy-AIO block')
            after = data.replace(old, b'    # Experimental 4.9 recovery: native FunctionFS AIO and configfs.\n'
                                      b'    setprop ro.adb.nonblocking_ffs 1\n'
                                      b'    setprop sys.usb.controller msm_hsusb\n')
        if after != data:
            changes[name] = {'before_sha256': digest(data), 'after_sha256': digest(after)}
            fields = fields.copy()
            fields[6] = len(after)
        result.append((name, fields, after))
    require(set(changes) == {'init.rc', 'init.recovery.qcom.rc'}, 'Unexpected ramdisk change set')
    props = dict((n, d) for n, _, d in result)
    require(b'ro.debuggable=1\n' in props['prop.default'], 'Recovery ADB would not auto-start')
    require(b'ro.adb.secure.recovery 0' in props['init.recovery.qcom.rc'], 'Missing recovery root ADB')
    require(b'nonblocking_ffs 0' not in props['init.recovery.qcom.rc'], 'Legacy AIO remains forced')
    return result, changes


def dt_entries(data):
    require(len(data) >= 36 and data[:4] == b'QCDT', 'Bad QCDT magic')
    version, count = struct.unpack_from('<II', data, 4)
    require(version == 2 and count == 1, 'Not the measured single-entry QCDT v2')
    entry = struct.unpack_from('<6I', data, 12)
    require(entry[:4] == (264, 8, 9, 0), 'Changed C5 bootloader DT selection identity')
    off, size = entry[4:]
    require(off == 2048 and size > 40 and off + size == len(data), 'Bad DT extent')
    blob = data[off:]
    magic, total = struct.unpack_from('>II', blob)
    require(magic == 0xd00dfeed and 40 <= total <= size and not any(blob[total:]), 'Bad DT payload')
    return entry, blob[:total]


def validate_kernel(build):
    root = build.parent
    result = (root / 'result.txt').read_text()
    require('exit_code=0\n' in result, 'Full board build has not succeeded')
    cfg = (build / '.config').read_text()
    require(digest(cfg.encode()) == CONFIG_SHA, 'Unreviewed board configuration')
    values = dict(re.findall(r'^CONFIG_(\w+)=(.*)$', cfg, re.M))
    require(all(values.get(key) == 'y' for key in REQUIRED), 'Required C5 recovery config missing')
    require(values.get('COMPILE_TEST') is None and values.get('MODULES') is None, 'Diagnostic/module config')
    require(values.get('NR_CPUS') == '8', 'Wrong C5 CPU count')
    fragment = (root / 'metadata/board.fragment').read_text()
    for key, value in re.findall(r'^CONFIG_(\w+)=(.*)$', fragment, re.M):
        require(values.get(key) == value, f'Requested board option normalized away: {key}')
    for key in re.findall(r'^# CONFIG_(\w+) is not set$', fragment, re.M):
        require(key not in values, f'Requested board disable not honored: {key}')
    release = (build / 'include/config/kernel.release').read_text().strip()
    require(release == '4.9.337-c5lte-board-candidate', 'Not the requested 4.9.337 board kernel')
    elf = (build / 'vmlinux').read_bytes()
    require(elf[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', elf, 18)[0] == 183,
            'Not an ARM64 kernel ELF')
    image = (build / 'arch/arm64/boot/Image.gz').read_bytes()
    raw = gzip.decompress(image)
    require(raw[56:60] == b'ARM\x64' and b'Linux version 4.9.337-c5lte-board-candidate' in raw,
            'Not a real ARM64 4.9.337 Image.gz')
    # Actual driver compilation, rather than config names alone.
    sources = '\n'.join(p.read_text() for p in build.rglob('.*.cmd') if p.is_file())
    for name in ('sec_battery_v2', 'sm5705_charger', 'sm5705_fuelgauge', 'da9155_charger',
                 'c5-msm-tsens', 'msm_thermal', 'muic_task', 'sdhci-msm', 'ci13xxx_udc'):
        require(f'/{name}.c' in sources, f'Required driver was not compiled: {name}')
    return image, {'release': release, 'config_sha256': digest(cfg.encode()),
                   'vmlinux_sha256': digest(elf), 'Image.gz_sha256': digest(image)}


def validate_source(root, expected_symlinks_sha=SOURCE_LINKS_SHA, expected_inventory_sha=SOURCE_INVENTORY_SHA):
    """Check the frozen tree against the build's complete, path-safe inventory."""
    inventory = root / 'metadata/source.sha256'
    require(digest(inventory.read_bytes()) == expected_inventory_sha, 'Unreviewed source inventory')
    source = (root / 'source').resolve()
    names = set()
    for line in inventory.read_text().splitlines():
        require(re.fullmatch(r'[0-9a-f]{64}  \./[^\n]+', line), 'Malformed source inventory')
        sha, name = line.split('  ', 1)
        path = source / name
        require(path.resolve().is_relative_to(source) and '..' not in PurePosixPath(name).parts,
                'Unsafe source inventory path')
        require(name not in names and path.is_file() and not path.is_symlink(), 'Bad source entry')
        require(digest(path.read_bytes()) == sha, f'Frozen source changed: {name}')
        names.add(name)
    paths = list(source.rglob('*'))
    actual = {'./' + str(p.relative_to(source)) for p in paths if p.is_file() and not p.is_symlink()}
    require(names == actual and bool(names), 'Incomplete source inventory')
    links = {str(p.relative_to(source)): os.readlink(p) for p in paths if p.is_symlink()}
    link_sha = digest(json.dumps(links, sort_keys=True, separators=(',', ':')).encode())
    require(link_sha == expected_symlinks_sha, 'Unexpected source symlink inventory')
    require(all((source / name).resolve().is_relative_to(source) for name in links), 'Source symlink escape')
    return {'files': len(names), 'inventory_sha256': digest(inventory.read_bytes()),
            'symlink_count': len(links), 'symlink_inventory_sha256': link_sha}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel-build', required=True, type=Path)
    parser.add_argument('--board-dtb', required=True, type=Path)
    parser.add_argument('--rescue-image', required=True, type=Path)
    parser.add_argument('--mkbootimg', required=True, type=Path)
    parser.add_argument('--dtbtool', required=True, type=Path)
    parser.add_argument('--dtc-dir', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    require(not args.output.exists() and not args.output.is_symlink(), 'Output must be new')
    require(not args.output.resolve().is_relative_to(repo), 'Artifacts must stay outside Git')
    rescue = args.rescue_image.read_bytes()
    require(digest(rescue) == RESCUE_SHA, 'Rescue-image hash mismatch')
    require(digest(args.mkbootimg.read_bytes()) == MKBOOTIMG_SHA, 'mkbootimg hash mismatch')
    require(digest(args.dtbtool.read_bytes()) == DTBTOOL_SHA, 'dtbTool hash mismatch')
    require(digest((args.dtc_dir / 'dtc').read_bytes()) == DTC_SHA, 'DTC hash mismatch')
    board = args.board_dtb.read_bytes()
    require(digest(board) == DTB_SHA, 'Unreviewed full-board DTB')
    image, kernel_report = validate_kernel(args.kernel_build)
    source_report = validate_source(args.kernel_build.parent)
    # V1 passes packaging checks but fails the subsequently discovered memory
    # contract. Reject before creating output, never waive this for a build.
    validate_memory_contract(args.board_dtb, (args.kernel_build / '.config').read_text())
    before = boot_parts(rescue)
    dt_entries(before['dt'])
    ramdisk_entries, changes = native_ramdisk(cpio_read(gzip.decompress(before['ramdisk'])))
    packed = gzip.compress(cpio_write(ramdisk_entries), compresslevel=9, mtime=0)
    args.output.mkdir(mode=0o700)
    inputs = args.output / 'inputs'
    inputs.mkdir()
    (inputs / 'Image.gz').write_bytes(image)
    (inputs / 'ramdisk.gz').write_bytes(packed)
    dt_dir = inputs / 'dtb'
    dt_dir.mkdir()
    (dt_dir / args.board_dtb.name).write_bytes(board)
    dt_img = inputs / 'dt.img'
    commands = [[str(args.dtbtool), '-2', '-o', str(dt_img), '-s', '2048',
                 '-p', str(args.dtc_dir) + '/', str(dt_dir) + '/']]
    subprocess.run(commands[0], check=True)
    _, new_dtb = dt_entries(dt_img.read_bytes())
    require(new_dtb == board, 'DT tool changed the reviewed DTB')
    os_value = before['fields'][9]
    os_version = f'{os_value >> 25 & 127}.{os_value >> 18 & 127}.{os_value >> 11 & 127}'
    patch = f'{2000 + (os_value >> 4 & 127):04d}-{os_value & 15:02d}-01'
    output = args.output / 'recovery-4.9.337-c5lte-EXPERIMENTAL.img'
    command = [str(args.mkbootimg), '--kernel', str(inputs / 'Image.gz'), '--ramdisk',
               str(inputs / 'ramdisk.gz'), '--dt', str(dt_img), '--base', '0x80000000',
               '--kernel_offset', '0x00008000', '--ramdisk_offset', '0x02000000',
               '--second_offset', '0x00f00000',
               '--tags_offset', '0x01e00000', '--pagesize', '2048', '--header_version', '0',
               '--cmdline', before['cmdline'], '--os_version', os_version, '--os_patch_level',
               patch, '--output', str(output)]
    commands.append(command)
    subprocess.run(command, check=True)
    with output.open('ab') as stream:
        stream.write(b'SEANDROIDENFORCE')
    artifact = output.read_bytes()
    after = boot_parts(artifact)
    require(after['kernel'] == image and after['ramdisk'] == packed and after['dt'] == dt_img.read_bytes(),
            'Packaged payload mismatch')
    require(after['cmdline'] == before['cmdline'] and after['fields'][9] == os_value,
            'Measured command line/OS field changed')
    require(len(artifact) <= 33554432, 'RECOVERY partition capacity exceeded')
    dt_entries(after['dt'])
    require(cpio_read(gzip.decompress(after['ramdisk'])) == ramdisk_entries, 'CPIO metadata changed')
    sha = hashlib.sha1()
    for payload in (image, packed, b'', dt_img.read_bytes()):
        sha.update(payload)
        sha.update(struct.pack('<I', len(payload)))
    require(after['id'] == sha.digest() + b'\0' * 12, 'Boot payload ID mismatch')
    report = {'result': 'BUILT_NOT_BOOT_VERIFIED_NOT_AUTHORIZED_TO_FLASH',
              'hardware': 'NOT TESTED', 'profile': 'RECOVERY_ONLY_NOT_A_SYSTEM_KERNEL',
              'rescue_image_sha256': RESCUE_SHA, 'kernel': kernel_report,
              'source': source_report, 'tools': {'mkbootimg_sha256': MKBOOTIMG_SHA,
                                               'dtbTool_sha256': DTBTOOL_SHA, 'dtc_sha256': DTC_SHA},
              'packager_sha256': digest(Path(__file__).read_bytes()),
              'board_dtb_sha256': DTB_SHA, 'ramdisk_changes': changes, 'commands': commands,
              'artifact': str(output), 'bytes': len(artifact), 'sha256': digest(artifact)}
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    (args.output / 'SHA256SUMS').write_text(f'{digest(artifact)}  {output.name}\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'FAIL: {error}') from error
