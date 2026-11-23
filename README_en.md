<p align="center">
  <img src="docs/icon.png" width="120" height="120" alt="Henge">
</p>

<h1 align="center">Henge — Android Port</h1>

<p align="center">Run macOS on your own Android phone.</p>

<p align="center">
  <a href="#install">Install</a>
  ·
  <a href="#what-this-fork-does">What this fork does</a>
  ·
  <a href="#building">Building</a>
  ·
  <a href="docs/architecture.md">How it works</a>
  ·
  <a href="THIRD_PARTY.md">Third-party</a>
  ·
  <a href="README.md">中文</a>
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--2.0--or--later-2E6BFF?style=flat-square&labelColor=111111" alt="GPL-2.0-or-later"></a>
</p>

> **This repository is not upstream.**
> Upstream is [Leviidev/Henge](https://github.com/Leviidev/Henge) by Leviidev, which targets **iPhone and iPad**.
> This fork ports the same engine to **Android arm64** on the `dev` branch, and continues kernel-boot and
> GPU-rendering work there. Upstream's `main` is the iOS line; our work lives on `dev`.

---

## Scope of use — read this

- **Personal research use only; non-commercial.**
- **macOS is governed by Apple's Software Licence Agreement (SLA), and this project's use of it is outside what
  Apple grants.** SLA §2B(iii) allows virtualising macOS only on **Apple-branded hardware that is already
  running macOS**, **at most two** additional instances per machine, and only for software development, testing
  during development, macOS Server, or **personal, non-commercial use**. SLA §2J is blunter: you may not
  install, use or run macOS on **any non-Apple-branded computer**.
- **So running macOS on Android is the same kind of personal hacker practice as a community “Hackintosh” —
  tinker on your own hardware, at your own risk. We do not pretend this is compliant, and nothing here
  grants any Apple permission.**
- **This project provides no macOS image and does not redistribute the Boot ROM.** **Commercial use, packaged
  redistribution and sublicensing are prohibited.**
- **Users must read and comply with Apple's SLA themselves.** Nothing here is a licence or legal advice.

---

## Test scope — read this first

**This Android port has been tested on exactly one device:**

| | Tested device |
|---|---|
| Model / board | **PJE110 (OnePlus)** / `kalama` (**SM8550**) |
| GPU | **Adreno 740** |
| OS | **Android 15** |
| Page size | host **4 KB** pages (the guest still runs **16 KiB** pages) |

On that device, **the install path (DFU + idevicerestore) completes**:

```
install: restore finished: 0
step 100 (100%)
disk.img  68,719,476,736 logical bytes (64 GiB sparse, ~19.8 GB on disk)
```

**This does not mean other devices will work.** That is not boilerplate — there are concrete reasons:

- **The GPU path is narrow.** It runs `PVG device model → reims-vgpu → metal2vulkan → Vulkan → SurfaceView`.
  One stage needs `VK_EXT_external_memory_host` plus dma_buf. Measured here: the **vendor Adreno driver
  fails outright** (no `external_memory_host`) and **only Turnip works**. On another phone this stage may
  not come up at all.
- **Page size is a hard constraint.** The host-4 KB / guest-16 KiB combination is what was brought up;
  some diagnostics (page-table write history, for one) read wrong under other page sizes.
- **Capabilities are probed at run time, not assumed at build time** — but a failed probe degrades or
  stops, it does not adapt itself.
- **Each install creates a 64 GiB image.** Not enough storage means the process never finishes.
- **Only AArch64 was ever run.** An `armeabi-v7a` target exists in the build scripts and has never been tested.

**Treat this repository as a port that was validated on one machine, not as a general Android release.**

If it fails elsewhere, **the most likely cause is the JIT translator not being compatible with that chip's
instruction set — not the GPU.** "It is arm64 too" does **not** mean it runs the same:

- The JIT emits native instructions directly, and what it may assume differs between SoCs: the
  instruction-set floor, PAC / non-canonical-address details, cache maintenance and the observable
  behaviour of self-modifying code, SIMD behaviour, and which system registers are visible.
- Those differences only show up when you **measure on that specific chip**; whether to special-case
  them or route around them is a per-SoC sweep, one device at a time.
- Today's JIT path has been validated against **SM8550 (a Cortex-X3 / A715 / A510 part) only.** Another
  chip needs **its own testing and its own adjustments**; isomorphism cannot be assumed.

GPU capabilities and page size are real constraints, but they are the layer **after** we got the install
through on this one device — they are not the first explanation for why someone else's phone fails.
Keep the layers apart:

```
JIT / instruction-set compatibility  →  does the engine run at all   (precondition, most likely to break)
GPU / page size                      →  is the picture right        (only once it runs)
```

---

## What this fork does

Starting point: the iOS version works (macOS reaches the lock screen in about 15 seconds on an iPhone 17 Pro).
The port started on **2026-11-13**; the first commit is `bb36f6f` —
*Android port: engine builds and runs, install stack, UI, driver loading*.

### 1. The engine barely needed changes

The JIT translator, the software MMU and the device models are plain user-space C and platform-agnostic:

| Module | Port changes |
|---|---|
| `core/` (JIT, dispatcher, exceptions, MMU) | almost none; only ABI-level fixes |
| `devices/` (GICv3, PL011, PL031, PL061, AES, cfg, bdif, xHCI, PVG, virtio) | none |
| `host/` (threads, kicker, WFI, disk, USB shim) | platform parts replaced |

Only the host layer and the entry points are new (`android/install.c`, `android/henge_vm.c`, `android/probe_jni.c`).

### 2. Three real bugs found during the port (none of them "platform incompatibility")

- **`x16` polluted by a host GOT address** (commit `27ab79c`). `x16` is both a guest register and the
  AArch64 IP0; when C code takes the address of an assembly entry point it goes through the GOT/PLT,
  and that sequence uses `x16` as a scratch register. The host `.got.plt` address then landed in the
  guest's authoritative slot. **Fix:** mark every assembly symbol the JIT uses `.hidden`, and delete the
  one dead field that took the address of `hv_helper_entry`.
  **Effect:** the install moved from "crashes the moment iBSS starts" to reaching the kernel.
  Regression test: `android/test-regs.sh` (35 cases, host-side, sub-second).
- **SIMD clobber:** `put_helper` / `refill` did not save `v0-v31`, so the first SIMD instruction in the
  kernel destroyed JIT state.
- **Wrong time base** (commit `4720d2e`). `counter()` returned the host's `CLOCK_MONOTONIC_RAW` scaled
  value, i.e. **measured from host boot**, while the guest's commpage offset read 0; guest uptime
  overflowed to negative and the clock drifted to 2087. **Apple's personalised restore material carries
  time windows**, and a clock outside them does not validate. Changing it to "lock the base on first
  call, return deltas afterwards" moved the restore from stuck at `set_progress_0` to **ASR 90%**, and
  eventually to `restore finished: 0`.

> The easiest wrong turn here: **in Apple's world, time is not a display concern, it is a precondition
> for signatures.** That is nothing like Linux, where a skewed clock mostly just looks bad in logs.

### 3. Imported / ported components

Newly introduced under `third_party/` (the iOS build either does not need them or carries them differently):

| Component | Purpose |
|---|---|
| `idevicerestore` / `libimobiledevice` / `libirecovery` / `libplist` / `libimobiledevice-glue` / `libtatsu` / `libusbmuxd` | Apple's restore protocol (DFU / recovery / restore) |
| `usbmuxd` + `libusb-1.0` | Virtual USB stack in the guest (`host/libusb_shim.c`) |
| `mbedtls` | TLS for idevicerestore |
| `libzip` + `lzfse` | IPSW extraction and LZFSE decompression |
| `curl` | Downloading IPSWs from Apple |
| `restore_config` | Restore configuration material |

Under `android/third_party/`, the graphics chain:

| Component | Purpose |
|---|---|
| `reims-vgpu` | Rust implementation of the PVG device model (Apple's paravirtual GPU protocol) |
| `metal2vulkan-runtime` (m2v) | Metal command → Vulkan translation |
| `libadrenotools` | Namespace-loading of the Vulkan driver (Turnip), bypassing the system driver |
| `lzfse` | LZFSE on the Android side |

The Vulkan driver itself (**Turnip 1.3.296**) is **downloaded, not committed**; `PROVENANCE.txt` records
what it was.

### 4. The Android app

`android/app/` is a complete native Android app (Java + Material, package `dev.henge.probe`):

- **Install tab:** download or import an IPSW, verify it, run the install (foreground service + notification)
- **Macs tab:** start an installed macOS
- **DisplayActivity:** a `SurfaceView` carrying the guest's frames, with input forwarding (touch behaves as a trackpad)
- **Settings / Logs / Diagnostics:** log level, driver selection, diagnostics
- **Driver loading:** Turnip via libadrenotools' namespace, or the system driver

### 5. Logging architecture (deliberate — do not work around it)

Engine stderr → log-service thread → **①the full stream to `files/vm/henge.log`** (GB-scale) plus
**②rate-limited samples to logcat** (`HENGE_LOG_RATE`, default 120 lines/s; keyword lines are exempt).

**Do not log to logcat at high frequency from the engine.** Android applies a per-process logcat quota,
and blowing it drops the lines that matter — that already happened once here, and it looked exactly like
"this code never ran" when in fact the log had been swallowed.

### 6. Where it stands

| Stage | Status |
|---|---|
| Engine builds and runs (arm64, bionic) | ✅ |
| Install macOS (DFU + idevicerestore) | ✅ `restore finished: 0`, `step 100 (100%)` |
| Installed system boots to lock/login screen | ✅ |
| Kernel serial log (`serial=3`) | ✅ |
| Present path reaches the surface (swapchain, `host presents` 0 → 109) | ✅ |
| **Correct picture content** | ⚠️ **Not done.** Several layers composite wrongly today (background drawn as 16×4 bricks, an atlas texture read linearly, …). See `docs/gpu-layer-corruption-2026-11-22.md` |

**The main goal is "an installed macOS boots to a correctly rendered screen."** Everything else gives way.
The UI is not being worked on until the install path is complete; today it exists to drive the install and
to observe.

---

## Install

> For now you have to build the APK yourself (see below). There are no releases.
> One instance per device: a single instance has an RSS of about 2.1 GB, and an install writes a 64 GiB
> sparse image.

On the device:

1. **Storage:** at least 25 GB free (IPSW ~16 GB plus the sparse disk's ~20 GB of real usage).
2. **IPSW:** an `*.ipsw` from Apple (tested: `24G90`).
3. **Booter:** `AVPBooter.vmapple2.bin`, produced by the same build as the APK.

Then: install the APK → open it → **Install** tab, pick the IPSW → start → wait for `step 100 (100%)` →
go back to **Macs** and start it.

---

## Building

### Environment (measured on the reference machine, for reference)

| Dependency | Version |
|---|---|
| Android NDK | **r29** (`29.0.14206865`) |
| Host | **linux-arm64** (native, no x86_64 emulation) |
| JDK | 21 |
| Gradle / AGP | 8.13 / 8.13.2 |
| Rust (`reims-vgpu`) | 1.98.1, target `aarch64-linux-android` |
| Other | cmake 3.28 / ninja 1.11 / make 4.3 |

Target defaults: ABI `arm64-v8a`, API 33 (`minSdk 26`, `compileSdk 36`, `targetSdk 36`).

### Three steps

```bash
source android/env.sh        # NDK plus both toolchains
./android/build-restore.sh   # idevicerestore / libimobiledevice (rarely needs rebuilding)
./android/build-c.sh         # engine + JNI → libhenge.so
./android/build-apk.sh       # package the APK
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell am force-stop dev.henge.probe   # the app must be restarted after installing
```

`android/README.md` has the detailed environment notes, and `docs/debug-switches.md` is the single table of
environment switches (logging, boot, breakpoints, userland, USB, devices, JIT, install).

### Run without a device (start here)

The host CLI turns around in tens of seconds and needs no phone:

```bash
./tools/build-install-host.sh
CURL_CA_BUNDLE=$PWD/work/ca-bundle.pem \
  ./build/host/host-install work/AVPBooter.vmapple2.bin android/ipsw/24G90.ipsw work 2048 2
```

**Host and device differ materially — say so whenever a conclusion only holds on the host:**

- **PVG is a stub** (real graphics only exist in `android/`)
- different RAM and CPU count
- the host has **4 KB** pages (the guest still has 16 KiB)

### Run the CLI on the device (1–3 minutes per round, unattended)

```bash
ZLIB=1 API=28 ./tools/build-install-android.sh
adb -s <serial> push build/andcli/host-install /data/local/tmp/henge/
adb -s <serial> push work/AVPBooter.vmapple2.bin /data/local/tmp/henge/
adb -s <serial> shell "cd /data/local/tmp/henge && chmod 755 host-install && \
  ./host-install AVPBooter.vmapple2.bin /data/local/tmp/ipsw/24G90.ipsw workN 2048 2 > run.log 2>&1"
```

Argument order: `booter ipsw workdir RAM_MB loglevel`. **A missing argument misbehaves silently**
(`workN 3` reads 3 as RAM=3 MB: the machine fails to come up and nothing is reported).
**Use a fresh `workN` directory every time.**

### Unit tests (sub-second; run before and after touching the JIT)

```bash
./android/test-regs.sh
./android/test-bitops.sh     # ONLY=bitops|memops|mathops|atomics  ROUNDS=N
./android/test-lzfse.sh
```

---

## Known limits

- **Tested only on PJE110 / SM8550 / Adreno 740 / Android 15.** Other devices are not guaranteed.
- **Graphics content is currently composited wrongly**: background, atlas textures and other layers are
  not drawn correctly yet (see the table above).
- **The GPU path needs Turnip**: the vendor driver lacks `VK_EXT_external_memory_host` and fails outright here.
- **arm64 only**, `minSdk 26`.
- **One instance at a time**, and a fresh work directory per round.
- **Install runs once**: the resulting `disk.img` (64 GiB logical / ~20 GB real) is an asset — do not delete
  it, and do not re-run an install over it.
- **Host 4 KB pages vs guest 16 KiB pages**: computing guest page-table indices with the 4 KB / 4-level
  formula gives entirely wrong results.
- This is software emulation; it runs much slower than a real Mac.

---

## Documentation (read first when taking over)

| Document | Contents |
|---|---|
| `docs/investigation-tree.md` | **Master index**: branches explored, conclusions that hold, conclusions that fell, methodological rules |
| `docs/exclusions-current-stall-2026-11-19.md` | Exclusion list for the current stall (43 items ruled out) |
| `docs/gpu-render-bringup-2026-11-21.md` | Bringing the GPU render path up |
| `docs/gpu-layer-corruption-2026-11-22.md` | Locating the current compositing fault |
| `docs/kernel-boot-exclusions.md` | Kernel-boot hypotheses ruled out, E1–E18 |
| `docs/debug-switches.md` | **The single table of environment switches** |
| `docs/android-install-debug.md` / `android-install-status.md` | Android install-chain debugging and status |
| `docs/architecture.md` | Engine architecture |
| **`THIRD_PARTY.md`** | **The list of everything imported**: each component's repository link, how it is carried, its licence, and whether we changed it |
| `docs/licensing.md` | Licensing and the combined-work judgement, in full |

**A new environment variable (`getenv`) must be added to `docs/debug-switches.md` in the same change**;
that file is the single source of truth for which switches exist.

*(The documents themselves are written in Chinese.)*

---

## Licence

**Personal research use only; non-commercial.** macOS is governed by Apple's SLA, and this project's use of it
**falls outside what Apple grants** (§2B(iii) requires an Apple-branded host that is already running macOS, at most
two extra VM instances per machine, and only four permitted purposes; §2J forbids running macOS on any
non-Apple-branded computer). This is the same category of personal hacker practice as a community “Hackintosh”:
tinker on your own hardware, at your own risk. We do not pretend it is compliant, and nothing here grants Apple
permission. This project **provides no macOS image and does not redistribute the Boot ROM**; **commercial use,
packaged redistribution and sublicensing are prohibited**. Users must read and comply with Apple's SLA themselves.

**GPL-2.0-or-later**, same as upstream. Henge compiles in usbmuxd (GPLv2 or GPLv3) and code from Husk
(GPL-2.0-or-later), so the shipped binary is a combined GPL work and **cannot go on the App Store**.

macOS and the Mac's boot ROM are Apple's and are not part of this repository; Henge downloads both from
Apple at install time.

## Credits

### How this was built

The Android port in this repository **was written by artificial intelligence**. The development agent is the
coding agent from **[pi.dev](https://pi.dev)**, which is also the framework that actually executed the work;
the AI model comes from **DeepSeek** (`deepseek-flash`, called through the deepseek.com API), with a small
amount of advice from free web models on **OpenAI** and **claude.ai**.
In short: **every part of the port was carried out by AI, and the harness that ran the commands, edited the
code and ran the experiments was pi.dev.**

> This is not a disclaimer: conclusions are still judged by the measurements recorded under `docs/`, not by
> who said them. Anything derived rather than measured is marked uncertain unless source code or a test
> backs it up.

### Upstream and dependencies

- **[Leviidev/Henge](https://github.com/Leviidev/Henge)** — the upstream engine and the iOS app. Every
  part of the core work here is theirs.
- **QEMU**'s `hw/vmapple/*` — the reference the device models were checked against.
- **mesa / Turnip**, **libadrenotools**, **reims-vgpu**, **metal2vulkan** — the graphics chain.
- **The libimobiledevice community** — the restore stack.
