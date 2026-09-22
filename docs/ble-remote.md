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

### Heap and lifecycle — measured on hardware

Measured on the X4 Pro (base MAC `B8:1F:3F:D5:03:D0`) over the pogo USB
adapter, running `x4-pro-blespike`. The probe fires about 2.5 s into boot,
just after the first panel refresh completes, so these are post-boot figures
with the display up and the SD card mounted but no book open.

All values are bytes, from `heap_caps_get_free_size()` and
`heap_caps_get_largest_free_block()` with `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`.

| Phase | Internal free | Internal largest block | DMA-capable free | PSRAM free |
| --- | ---: | ---: | ---: | ---: |
| Before first `init()` | 189,736 | 147,444 | 182,072 | 8,266,036 |
| After `init()` + advertising | 122,900 | 81,908 | 115,236 | 8,266,036 |
| After `deinit(true)` | 189,488 | 114,676 | 181,824 | 8,266,036 |
| After ten more init/deinit cycles | 189,488 | 114,676 | 181,824 | 8,266,036 |

Ten further cycles reported an identical `122,900` while up and `189,488`
after teardown on **every** iteration, with no drift in either direction. Two
independent boots produced byte-for-byte identical figures, so these are
reproducible rather than a single lucky sample.

| Question | Answer |
| --- | --- |
| Does `NimBLEDevice::deinit(true)` return memory? | **Yes.** Internal free returns to within 248 B of the pre-init figure. |
| Does a later `init()` succeed on this SDK? | **Yes.** Eleven consecutive init/deinit round trips, all successful. |
| Is controller memory release one-way? | **No.** The adapter may tear the stack down on activity exit rather than keeping it resident. |

#### What these numbers mean for the design

- **A live stack costs 66,836 B of internal RAM** (189,736 → 122,900). PSRAM
  free does not move at all across init, so PSRAM is genuinely not a
  substitute here, exactly as the spec warned. DMA-capable free drops by the
  same 66,836 B, so the cost is entirely in internal DRAM.
- **Teardown is real and does not compound.** 248 B is retained after the
  first `deinit(true)` and nothing further accumulates over ten more cycles.
  There is no leak to design around, and `BleRemote::end()` can genuinely free
  the stack on activity exit.
- **The first init permanently narrows the largest contiguous block by
  exactly 32,768 B** (147,444 → 114,676), even though free *size* comes back.
  A small permanent allocation lands inside what was the largest free region
  and splits it. Like the 248 B, this is a one-time cost: the largest block
  sits at 114,676 after cycle 1 and never moves again. Any code that needs a
  large contiguous internal buffer after Remote has been opened once in a
  session has 32 KB less room to work with.
- **Remote cannot run with a resident EPUB reader.** `.claude/CONTEXT.md`
  records a normal partial-reading session at roughly 85–90 KB internal free
  and ~49 KB largest block. A live stack needs 66,836 B of that. Entering
  Remote while a reader is still on the activity stack would leave under
  20 KB free, which is not survivable. Stage 5 must reach RemoteActivity by a
  route that has already torn the reader down, and must not push it on top of
  a live reader.

### Pairing and HID delivery — measured on hardware

Two sessions against the same Mac, peer `60:3e:5f:7d:51:a7`: a first-ever
pairing, then a reconnect to the stored bond after a reboot.

| | Session 1, first pairing | Session 2, reconnect |
| --- | ---: | ---: |
| Advertising start to `onConnect` | 8,799 ms | **799 ms** |
| Connect to authentication complete | 20,383 ms | already encrypted on connect |
| Security result | `encrypted=1 authenticated=1 bonded=1` | `encrypted=1 authenticated=1 bonded=1` |
| Reports emitted after encryption | 8 of 14 | **14 of 14** |
| Bonds stored | 1 | 1 |
| Disconnect on teardown | HCI 0x16, local host | HCI 0x16, local host |

`authenticated=1` in both sessions is the pass condition that matters: MITM
protection was achieved, so the link did **not** fall back to Just Works. The
pinned DisplayOnly + bonding + MITM + secure-connections configuration works
on macOS.

Host-side delivery was confirmed visually in session 2: TextEdit's cursor moved
ten characters right and then three left, exactly one step per `SEND` line, and
went idle after `releaseAll()`. No repeats, no missed steps, no stuck key.

Session 1's 8,799 ms is operator-paced — it includes finding the device in
System Settings. **799 ms is the real reconnect figure** the spec asks to be
recorded. macOS reconnected on its own with no prompt, and the bond survived
both `deinit(true)` and a reboot, confirming that bonds live in NVS and that
`deinit(clearAll = true)` only destroys the C++ wrapper objects.

In session 2 `onAuthenticationComplete` was logged *before* `onConnect`:
encryption was re-established from the stored bond fast enough that the two
callbacks interleaved on the host task. The adapter must not assume these
arrive in a fixed order.

#### Heap across connected sessions

| Phase | Internal free | Internal largest block |
| --- | ---: | ---: |
| Advertising, before connect | 122,900 | 81,908 |
| Session 1 connected, first bonding, after 14 reports | 122,008 | 77,812 |
| Session 1 after `deinit(true)` | 188,596 | 77,812 |
| Session 2 connected, existing bond, after 14 reports | 122,868 | 81,908 |
| Session 2 after `deinit(true)` | **189,488** | **114,676** |

Session 2 returns to exactly the advertise-only cycle baseline, byte for byte.
So the extra 892 B and the 36 KB contiguity loss seen in session 1 were the
**one-time cost of creating and persisting a new bond**, not a per-session
penalty. A connected session against an existing bond costs 32 B while live
and gives all of it back on teardown, with the largest block untouched.

This removes the concern raised after session 1. Tearing the stack down per
use remains viable; there is no need to keep it resident. Spec section 8's
fifty enter/exit cycles should still confirm this over a longer run, but on
the evidence so far nothing compounds.

#### Two spike artifacts, not product defects

- **Session 1 emitted six reports before the link was encrypted.** The probe
  starts its five-second focus grace at `onConnect`, but first-time
  authentication did not complete until 20 s later. Session 2, where the bond
  already existed, had encryption up 5 s before the first send and delivered
  all fourteen. The real adapter must gate sending on
  `onAuthenticationComplete`, not on connection.
- **`onPassKeyDisplay()` never fired.** `NimBLEDevice::setSecurityPasskey()`
  installs a fixed passkey that the stack uses directly, bypassing the
  callback. Stage 6 has to render the passkey on the e-ink screen, so it must
  either drop `setSecurityPasskey()` and return a generated code from
  `onPassKeyDisplay()`, or display the statically configured value itself.

### Build gate at Stage 0

All five targets built from `feat/ble-remote` at `9f0d4e70`:

| Target | Result | `firmware.bin` |
| --- | --- | ---: |
| `pio run -e x4-pro` | SUCCESS | 6,089,008 B |
| `pio run -e x4-pro-blespike` | SUCCESS | 6,259,152 B |
| `pio run -e x4-pro-simulator` | SUCCESS | native |
| `pio run -e default` | SUCCESS | 6,103,072 B |
| `pio run -e sticky` | SUCCESS | 5,974,368 B |
| `ctest` native suite | 539/539 passed | — |

`nm` over the `default` and `sticky` ELFs returns zero symbols matching
`nimble`, `ble_hs_`, `ble_gap_`, or `esp_bt_controller`, so the C3 and Sticky
images genuinely gain nothing.

### Deliberate differences from the spec

- **Bond count.** The prebuilt S3 SDK ships `CONFIG_BT_NIMBLE_MAX_BONDS=3`.
  Because NimBLE-Arduino compiles its own host, the build raises this to 4 from
  `build_flags`, which matches the spec's "at most 4 stored hosts". Bonds still
  live in the 20 KB `nvs` partition.
- **Device address — decided.** The reader advertises on the chip's public
  (eFuse MAC) address instead of the persisted static random address the spec
  asks for. A public address is already stable across reboots and power loss,
  which is the entire reason the spec wanted NVS persistence, so the NVS work
  is skipped. The X4 Pro used for Stage 0 has base MAC `B8:1F:3F:D5:03:D0`;
  ESP-IDF derives the Bluetooth address from that base.

---

### Stage 0 verdict

**Gate passed.** The image fits with 294,448 B to spare, `deinit(true)` returns
memory and re-init works with no progressive loss, authenticated bonded pairing
to macOS works, and HID reports reach the host one press per action.

Three findings carry into the later stages:

1. Gate sending on `onAuthenticationComplete`, never on `onConnect`, and do not
   assume the two callbacks arrive in a fixed order.
2. `onPassKeyDisplay()` does not fire while `setSecurityPasskey()` is set, so
   Stage 6 must generate the passkey through the callback to display it.
3. A live stack needs 66,836 B of internal DRAM and none of PSRAM, so
   RemoteActivity must not be entered with an EPUB reader still resident.

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
