# LineageOS 16 hardware test matrix

Last updated: 2026-10-04

Results apply only to the designated Samsung SM-C5000 with bootloader `C5000ZCU1CSA1` and the explicitly identified unofficial LineageOS 16 builds documented in `docs/build.md`. Unless a row names an earlier baseline, `PASS` means the stated test was observed on the currently installed CSA1 sensor-stack build or remains supported by unchanged components with a post-update sanity check. `PARTIAL` means only enumeration or a subset passed; `FAIL` means the tested functional scope did not work; `NOT TESTED` is not evidence of failure.

| Area | Status | Verified scope | Remaining scope |
| --- | --- | --- | --- |
| System boot | PASS | The Wi-Fi-fix rollback baseline passed initial boot, setup, launcher, normal reboots, suspend/resume, full power-off and cold boot; the first GNSS candidate failed, the second GNSS build completed boot in about 37 seconds, and the installed sensor-stack build completed boot in about 38 seconds with required services present under SELinux enforcing | Normal reboot and extended-duration regression on the sensor-stack build |
| Recovery | PASS | Display, touch, root ADB, sideload round trip, BCB mapping, required partitions | Long-term regression only |
| Display/touch | PASS | Setup, launcher/recovery interaction, automatic brightness, automatic rotation and screen-off/wake | Extended suspend cycles only |
| USB ADB | PASS | Android userdebug ADB, root ADB and recovery/sideload transports | Additional host/OS interoperability |
| USB MTP | PASS | Android reports `mtp,adb` for both configured and active gadget state while ADB remains available; the user completed a harmless file transfer round trip through the host file manager | Additional host/OS interoperability and extended transfers |
| Wi-Fi | PASS | Firmware/driver startup, discovery, user association, IPv4, default route, IP/DNS traffic, automatic reconnect after reboot | Extended stability, suspend/resume and hotspot |
| Bluetooth | PASS | Binder service/HAL, discovery, headset pairing/connection, A2DP media playback, power-cycle automatic reconnect and resumed playback | Headset microphone/HFP call audio and extended stability remain outside the tested scope |
| Audio | PASS | Phone speaker media output, built-in microphone recording, recording playback, Bluetooth A2DP media playback and wired-headset media output all work | Earpiece, wired-headset microphone, headset microphone and call audio remain outside the tested scope |
| Cameras | PASS | Front/rear preview and still capture, rear video recording/playback and flash all work; CameraService remains running with two normal devices and five video nodes | Extended stability and third-party app compatibility |
| Sensors | PASS | SensorService registers the expected entries; automatic rotation, automatic-brightness response and compass heading pass user testing; BHA250 accelerometer and YAS539 magnetic-field hardware self-tests pass. With the exact CSA1 sensor-hub firmware/HAL pair installed, Paseo counts steps, SensorService recorded 0-to-48 events and the kernel reported matching positive differences without the former invalid-FIFO errors. The proximity sensor responds correctly to cover/uncover transitions in phyphox Proximity timer | Gesture functional testing remains outside the tested scope |
| Internal storage | PASS | `/data` and `/cache` ext4 mounts are writable with expected capacity; system is read-only ext4; an MTP file transfer round trip passes | Adopted/removable storage and sustained I/O |
| Battery/USB power | PASS | Healthy battery state, USB charging, full power-off, offline charging screen and subsequent cold boot all work | Long charge progression, temperature under load and standby drain |
| NFC | PASS | Service/process/device node, enabled polling, phone-to-phone peer visibility and audible discovery of an external contactless card all work | Content reading, writing, HCE and payment are outside the tested scope; no card data was read |
| Fingerprint | PASS | Fingerprint service/HAL process, enrollment, lock-screen unlock and unlock after one-minute suspend all work under SELinux enforcing | Multiple-finger behavior and extended suspend stability |
| GPS/location | PASS | On the installed second CSA1 candidate, SatStat detected 17 satellites and Organic Maps identified the correct location; sanitized system state recorded 162 location reports, three TTFF reports with a 5.67-second mean, stable Samsung/QTI GNSS processes and no linker or fatal-process errors | Repeat cold-fix, normal-reboot and extended-stability tests without retaining coordinates |
| SIM/RIL/telephony | NOT TESTED | Phone, subscription and telecom binder services exist, but no SIM is installed | User explicitly declined SIM, data, SMS, call, call-audio and VoLTE testing for this device |
| SELinux | PASS | Enforcing after the second GNSS installation and during successful functional location testing | A GNSS-domain startup denial associated with an unused PeripheralManager registration path is a known non-blocking warning; continue AVC review during remaining tests |

No test should dial an emergency number. Logs committed to Git must not contain serial numbers, IMEI, phone numbers, wireless addresses, SSIDs, credentials or location history.
