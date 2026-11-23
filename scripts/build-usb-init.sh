#!/usr/bin/env bash
# Generate the four USB init files used by the tested SYSTEM.
set -euo pipefail

if [[ $# != 2 ]]; then
  echo "Usage: $0 ANDROID_SOURCE EXISTING_OUTPUT_PARENT" >&2
  exit 2
fi

scripts=$(cd -- "$(dirname -- "$0")" && pwd)
android=$(cd -- "$1" && pwd -P)
parent=$(cd -- "$2" && pwd -P)
if [[ "$parent" == "$android" || "$parent" == "$android/"* ]]; then
  echo "Choose an output directory outside the Android source tree" >&2
  exit 2
fi

out=$(mktemp -d "$parent/c5-usb-init.XXXXXXXX")
python3 - "$scripts" "$android" "$out" <<'PY'
import hashlib
import runpy
import sys
from pathlib import Path

scripts, android, out = map(Path, sys.argv[1:4])
overlay = runpy.run_path(str(scripts / "kernel-c5-android-usb.py"))
root = "system/core/rootdir/init.usb.rc"
configfs = "system/core/rootdir/init.usb.configfs.rc"
vendor = "device/samsung/c5lte/rootdir/etc/init.qcom.usb.rc"
inputs: dict[str, bytes] = {}
for name, pin in overlay["PINS"].items():
    raw = (android / name).read_bytes()
    if hashlib.sha256(raw).hexdigest() != pin:
        sys.exit(f"Unexpected Android source: {name}")
    inputs[name] = raw

generated, _, _ = overlay["transform"](
    inputs[root].decode(), inputs[configfs].decode(), inputs[vendor].decode()
)
profiles = runpy.run_path(str(scripts / "c5-usb-kernel-profiles.py"))
profile = profiles["transform"]({
    "current_root": generated["init.usb.rc"],
    "current_vendor": generated["vendor/etc/init/hw/init.qcom.usb.rc"],
    "legacy_root": inputs[root],
    "legacy_vendor": inputs[vendor],
})
(out / "init.usb.rc").write_bytes(profile["root"])
(out / "init.usb.configfs.rc").write_bytes(generated["init.usb.configfs.rc"])
usbd = (android / "system/core/usbd/usbd.rc").read_bytes()
if hashlib.sha256(usbd).hexdigest() != "772b27f3affb6e67d1a6bc83a2b201cbdc053854789326ac9833ab84277ff995":
    sys.exit("Unexpected Android source: system/core/usbd/usbd.rc")
vendor_rc = out / "vendor/etc/init/hw/init.qcom.usb.rc"
vendor_rc.parent.mkdir(parents=True)
vendor_rc.write_bytes(profile["vendor"])
usbd_rc = out / "system/etc/init/usbd.rc"
usbd_rc.parent.mkdir(parents=True)
usbd_rc.write_bytes(usbd)
PY

cd -- "$out"
sha256sum --check --status <<'PINS'
d5a19ba5ce59d8850abcf32508c7cee7fb464e36090d424c37e91399a834a195  init.usb.rc
5c2f614c05edc8cd6add0e28781d9f799f10e20e34962b318ffe6d55647fe4cc  init.usb.configfs.rc
PINS
patch --batch --forward --fuzz=0 --silent -p1 < "$scripts/../patches/lineage-17.1/usb-init/0001-add-midi-usb-compositions.patch"
sha256sum --check --status <<'PINS'
573e061103989f7f092ed18d4ed01240f2b4bd1d8faa78fe5b3b3552f72718f3  init.usb.rc
55dc5c82fe11009936ba67624b9a468d80fbc4f7b20bea88a53f62a006a9c74c  init.usb.configfs.rc
PINS
patch --batch --forward --fuzz=0 --silent -p1 < "$scripts/../patches/lineage-17.1/usb-init/0002-add-accessory-init.patch"
sha256sum --check --status <<'PINS'
f4c28368b6fc5656f87ba421ef95bd6b3720722ca15ff88dea0809f12cd2f495  vendor/etc/init/hw/init.qcom.usb.rc
6d1253a46e819ad8660e70521b31271c29177337cffdf47908a499725c31ba5d  system/etc/init/usbd.rc
PINS
patch --batch --forward --fuzz=0 --silent -p1 < "$scripts/../patches/lineage-17.1/usb-init/0003-add-audio-source.patch"
echo "e612f4f5261b767eced588c87d1412e11430fb9f39a373aefc78c7b2354739a8  init.usb.configfs.rc" | sha256sum -c --status
printf '%s\n' "$out"
