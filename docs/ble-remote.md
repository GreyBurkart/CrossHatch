# Bluetooth Remote (X4 Pro)

The Bluetooth Remote turns an Xteink X4 Pro into a small BLE HID control
surface. In a foreground **Remote** activity the reader presents itself to a
paired Mac, iPad, iPhone, or Windows PC as a keyboard and consumer-control
device: one physical button press or touch target sends exactly one key or
media action. Leaving the activity ends all Bluetooth behavior.

Direction is reader-to-host only. The host never drives CrossHatch. No
host-side software is required.

This feature is X4 Pro only, gated behind `CROSSINK_APP_CAP_BLE_REMOTE`. The
`default` (ESP32-C3) and `sticky` images build no Bluetooth code at all.

Specification: [`docs/ble-remote-spec.md`](ble-remote-spec.md).

---

## Stage 0 — size and lifecycle spike

Stage 0 is the gate that had to pass before any UI work. It answers three
questions: does the image still fit the OTA slot with NimBLE linked, what does
the stack cost in internal RAM, and does `NimBLEDevice::deinit(true)` actually
return memory and allow a later re-init on this SDK.

### Method

Branch `feat/ble-remote`, commit `cc98cd8d`, off `personal` at `bbed870c`.

- `h2zero/NimBLE-Arduino @ 2.5.1` pinned in `lib_deps` for `x4-pro` and
  `x4-pro-debug` only. `lib_ignore = BLE`, `board_build.arduino.memory_type =
  dio_opi`, `partitions.csv`, and the `freeink-sdk` submodule are unchanged.
- NimBLE-Arduino compiles its own copy of the NimBLE host rather than using the
  prebuilt SDK's, so the host build configuration is set from `build_flags`:
  `CONFIG_BT_NIMBLE_ROLE_CENTRAL=0`, `CONFIG_BT_NIMBLE_ROLE_OBSERVER=0`,
  `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`, `CONFIG_BT_NIMBLE_MAX_BONDS=4`.
  The prebuilt SDK supplies the controller only.
- A temporary `x4-pro-blespike` environment compiles `src/ble/BleRemoteSpike.*`,
  which runs once from `setup()` after the boot screen paints.

### Firmware size — measured on this machine

| Build | `firmware.bin` | Flash used | Free in the 0x640000 OTA slot |
| --- | ---: | ---: | ---: |
| `x4-pro` before this branch | 6,065,792 B | 6,065,289 B (92.5%) | 487,808 B |
| `x4-pro` with NimBLE in `lib_deps` but unreferenced | 6,089,008 B | 6,088,497 B (92.9%) | 464,592 B |
| `x4-pro-blespike`, NimBLE fully linked and exercised | 6,259,152 B | 6,258,649 B (95.5%) | **294,448 B** |

Delta from baseline to a fully-linked NimBLE image: **+193,360 bytes
(+188.8 KiB)**, about 3.0% of the OTA slot.

Static RAM (link-time `.data` + `.bss`, of 327,680 B):

| Build | Used | Delta |
| --- | ---: | ---: |
| `x4-pro` before this branch | 100,772 B | — |
| `x4-pro` with NimBLE unreferenced | 101,496 B | +724 B |
| `x4-pro-blespike` | 107,780 B | **+7,008 B** |

The image fits with roughly 288 KiB to spare, so `partitions.csv` does not
need to change and no X4 Pro user is forced into a full reflash. The Stage 4–9
feature code (adapter, activity, settings screen, strings) has to stay inside
that remaining headroom.

Note the middle row: simply listing NimBLE in `lib_deps` without referencing it
already costs about 23 KiB, because PlatformIO builds and links the declared
library. That cost lands only on X4 Pro images.

### Heap and lifecycle — requires hardware

**Not yet measured.** These numbers can only come from the device; the spike
binary below produces them. They are recorded here after the hardware run, and
nothing in this section is filled in from estimation.

| Phase | Internal free | Internal largest block |
| --- | ---: | ---: |
| Before first `init()` | _pending_ | _pending_ |
| After `init()` + advertising | _pending_ | _pending_ |
| After `deinit(true)` | _pending_ | _pending_ |
| After ten init/deinit cycles | _pending_ | _pending_ |

| Question | Answer |
| --- | --- |
| Does `NimBLEDevice::deinit(true)` return memory? | _pending_ |
| Does a later `init()` succeed on this SDK? | _pending_ |
| If release is one-way, keep the stack resident after first use | _pending_ |

For context when reading those numbers: a normal resume-into-partial reading
session on this hardware already sits at roughly 85–90 KB internal free and
~49 KB largest block (see `.claude/CONTEXT.md`). The Remote activity has to fit
in what is left at that point, not in the much larger boot-time headroom, so
the *delta* across init/deinit matters more than the absolute figures.

### Known deviations from the spec, to settle at Stage 6

- **Bond count.** The prebuilt S3 SDK ships `CONFIG_BT_NIMBLE_MAX_BONDS=3`.
  Because NimBLE-Arduino compiles its own host, the build raises this to 4 from
  `build_flags`, which matches the spec's "at most 4 stored hosts". Bonds still
  live in the 20 KB `nvs` partition.
- **Device address.** The spike advertises on the chip's public (eFuse MAC)
  address rather than a persisted static random address. A public address is
  already stable across reboots, which is what the spec's NVS requirement is
  for. Whether to switch to a persisted static random address is a Stage 6
  decision, not a Stage 0 blocker.

---

## Stage 0 hardware test script

Flash the spike image to the X4 Pro through the pogo USB adapter, pair it to a
Mac, and confirm one arrow key reaches Keynote. The probe is entirely
serial-driven; the reader's screen shows its normal boot destination
throughout and does not display the passkey in this Stage 0 build.

### 1. Flash

From the repository root, with the reader attached:

```bash
pio run -e x4-pro-blespike -t upload
```

If PlatformIO cannot find the port, flash the app partition directly. The
partition table is unchanged, so only the app image needs writing:

```bash
esptool.py --chip esp32s3 --port /dev/cu.usbmodem101 --baud 921600 write_flash 0x10000 .pio/build/x4-pro-blespike/firmware.bin
```

### 2. Open the serial log

```bash
pio device monitor -e x4-pro-blespike
```

Everything the probe reports is tagged `BLESPIKE`.

### 3. What the log should show

The probe runs immediately after the boot screen paints. Expected sequence:

```
INF BLESPIKE: ==== Stage 0 BLE spike start ====
INF BLESPIKE: HEAP pre-init           internalFree=... internalLargest=... dmaFree=... psramFree=...
INF BLESPIKE: HEAP post-init-1        internalFree=... internalLargest=...
INF BLESPIKE: HEAP post-deinit-1      internalFree=... internalLargest=...
INF BLESPIKE: CYCLE  2 internalFreeWhileUp=... internalFreeAfterDeinit=...
...
INF BLESPIKE: CYCLE 11 internalFreeWhileUp=... internalFreeAfterDeinit=...
INF BLESPIKE: HEAP post-10-cycles     internalFree=... internalLargest=...
INF BLESPIKE: re-init after deinit(true): WORKS
```

Read those lines as follows:

- `post-init-1` minus `pre-init` is the cost of a live stack. This is the
  number the Remote activity has to afford mid-session.
- `post-deinit-1` compared with `pre-init` says whether `deinit(true)` gives
  the memory back. If it returns to within a few hundred bytes, teardown is
  real. If it stays close to `post-init-1`, controller release is one-way and
  the adapter must keep the stack resident after first use.
- `CYCLE 2` through `CYCLE 11` must not trend downward. A steady drop of the
  same amount per cycle is a leak.
- `re-init after deinit(true): FAILED (one-way release)` means the second
  `init()` did not come back. That is a documented outcome, not a crash — the
  probe then skips the pairing window and returns to normal boot.

### 4. Pair to the Mac

After the cycle test the probe brings the stack up one last time and opens a
120-second pairing window:

```
INF BLESPIKE: HEAP post-init-final    internalFree=... internalLargest=...
INF BLESPIKE: PAIR window open for 120 s; device name 'CrossHatch Remote', passkey 424242
```

On the Mac: **System Settings → Bluetooth**, find **CrossHatch Remote**, click
Connect, and type **424242** when macOS asks for a code. macOS must ask for a
code — the reader declares DisplayOnly with bonding, MITM, and secure
connections, so a silent Just Works pairing would mean the security
configuration did not take effect. Report it if macOS pairs without prompting.

Expected:

```
INF BLESPIKE: EVENT connect peer=...
INF BLESPIKE: EVENT passkey-display passkey=424242
INF BLESPIKE: EVENT auth-complete encrypted=1 authenticated=1 bonded=1
INF BLESPIKE: PAIR connected after <n> ms; reconnect/connect time recorded
INF BLESPIKE: HEAP connected          internalFree=... internalLargest=...
INF BLESPIKE: Focus the target app now; first key in 5 s
```

`encrypted=1 authenticated=1 bonded=1` is the pass condition. `authenticated=0`
would mean the link came up unauthenticated.

### 5. Key delivery

You get five seconds after `Focus the target app now`. Put a Keynote deck into
presentation mode, or click into a TextEdit document, and leave it focused.

The probe then sends, one press and one release per action, three seconds
apart:

- 10 × Right Arrow
- 3 × Left Arrow
- 1 × consumer Play/Pause

```
INF BLESPIKE: SEND keyboard keycode=0x4F (press+release)
...
INF BLESPIKE: SEND keyboard keycode=0x50 (press+release)
...
INF BLESPIKE: SEND consumer usage=0x00CD (press+release)
INF BLESPIKE: releaseAll sent
INF BLESPIKE: HEAP after-send-burst   internalFree=... internalLargest=...
INF BLESPIKE: bondedHosts=1
INF BLESPIKE: HEAP post-final-deinit  internalFree=... internalLargest=...
INF BLESPIKE: ==== Stage 0 BLE spike end ====
```

What to watch for on the Mac:

- Keynote should advance exactly 10 slides, then go back exactly 3. In
  TextEdit the cursor should move 10 characters right, then 3 left.
- **Exactly one** step per `SEND` line. Two steps means the release report is
  being missed. Continuous movement means a key is stuck.
- After `releaseAll sent`, nothing further should happen. Hold a finger on the
  keyboard-free Mac for a few seconds and confirm the text field is idle.
- Play/Pause should toggle whatever media app is frontmost, or do nothing
  visible if none is. Either is fine; the log line is the real check.

### 6. What to send back

Paste the whole `BLESPIKE` block. The numbers that decide the gate are the
`HEAP` lines, the eleven `CYCLE` lines, and the `re-init after deinit(true)`
verdict. Also say whether macOS asked for the passkey and whether the arrow
keys moved one step each.

### 7. Afterwards

The spike leaves a real bond on the Mac. Before flashing the Stage 4–9
firmware, remove **CrossHatch Remote** from System Settings → Bluetooth, or the
Mac's stale bond will refuse to re-pair. Reflash the normal image with:

```bash
pio run -e x4-pro -t upload
```
