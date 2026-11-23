# Licensing

## Two layers, not one

**Code.** Henge and the Android port are **GPL-2.0-or-later** ([LICENSE](../LICENSE)). The shipped binary is a
combined GPL work — usbmuxd (GPLv2 or GPLv3) and code from Husk (GPL-2.0-or-later) are compiled into it — so it
cannot go on the App Store. Every imported component also remains bound by its own licence; this repository does
not relicense them (see the table below).

**System.** **macOS running inside the VM is Apple's**, in copyright and in licence, and is governed by
**Apple's Software Licence Agreement**. **This project holds no Apple authorisation, endorsement or support**,
and **provides no macOS image and no Boot ROM** — the device downloads them from Apple at install time.

## Scope of use

Personal research only, non-commercial. Apple's virtualisation grant (SLA §2B(iii)) requires an **Apple-branded
host that is already running macOS**, **at most two** additional instances per machine, and only four permitted
purposes (software development, testing during development, macOS Server, **personal non-commercial** use);
§2J forbids installing, using or running macOS on **any non-Apple-branded computer** at all. **Running macOS on
Android is therefore outside what Apple grants — the same category of personal hacker practice as a community
“Hackintosh”: the hardware and data are yours, and so is the risk and the responsibility.** Commercial use,
packaged redistribution and sublicensing are prohibited; users must read and comply with Apple's SLA themselves.
Nothing in this file is a licence or legal advice.

| Code | Licence | Where |
| --- | --- | --- |
| Henge | GPL-2.0-or-later | `core/`, `devices/`, `host/`, `ios/`, `tools/` |
| Code from Husk | GPL-2.0-or-later | `ios/Henge/JIT/`, `HengeLog.swift`, `Native/henge-ios-jit.*`, `henge-brk.S`, `HengeJITHelper/` |
| QEMU's vmapple AES device, as a reference | GPL-2.0-or-later | `devices/aes.c` |
| QEMU's `tcg-apple-jit.h` | LGPL-2.1-or-later | `ios/Henge/Native/` |
| usbmuxd | GPL-2.0 or GPL-3.0 | `third_party/usbmuxd` |
| idevicerestore | LGPL (see below) | `third_party/idevicerestore` |
| libimobiledevice, libplist | LGPL-2.1, with GPL-2.0 for their tools | `third_party/` |
| libimobiledevice-glue, libirecovery, libtatsu, libusbmuxd | LGPL-2.1 | `third_party/` |
| libzip | BSD | `third_party/libzip` |
| Mbed TLS | Apache-2.0 or GPL-2.0-or-later | `third_party/mbedtls` |
| StikJIT (the Bitrise build applies `ci/stikjit-high-address.patch`) | MPL-2.0 | `ios/Henge/Resources/legal/` |
| idevice | MIT | `ios/Henge/Resources/legal/` |
| Pairing library's Rust crates | Listed per crate | `ios/Henge/Resources/legal/LICENSES-rppairing-crates.txt` |
| libslirp, pcre2 | BSD | Prebuilt from Husk |
| glib, libintl | LGPL-2.1-or-later | Prebuilt from Husk |

idevicerestore's `COPYING` file is LGPL-3.0, while its source headers say LGPL-2.1
or later. If the `COPYING` file governs, the combined binary is GPL-3.0, which
GPL-2.0-or-later allows.

## Not covered

These are Apple's, and none of the licences above apply to them:

- **macOS**, which Henge downloads from Apple when you install it.
- **The boot ROM**, `AVPBooter.vmapple2.bin`. Henge fetches it on the device, from Apple's
  macOS 14.6.1 restore image, when you install. It isn't in this repository or in the app.

An exported Mac is a copy of that Mac, so its folder contains macOS and the ROM, as does
one made by `tools/export_ios.sh`. Those folders are for your own devices, not for sharing —
and redistributing them would breach Apple's licence regardless of GPL.
