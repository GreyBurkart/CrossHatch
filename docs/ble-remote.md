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

## How it works

### Using it

Settings > System > Bluetooth Remote.

- **Open Remote** advertises to hosts already paired for 30 seconds, then shows
  "Not connected".
- **Pair New Device** advertises openly for 60 seconds. The reader shows a
  six-digit code; type it on the host. The host must ask for the code — the
  reader declares DisplayOnly with bonding, MITM, and secure connections, so a
  silent Just Works pairing is refused.
- **Profile** cycles Presentation, Media, Navigation, and Custom.
- **Custom Buttons** assigns one action per slot from the v1 whitelist.
  Choosing one also switches the active profile to Custom.
- **Keep Awake While Connected** is off by default. When on, the sleep timer is
  suspended only while a host is connected and Remote is in the foreground.
- **Paired Devices** lists up to four bonded hosts with Forget. Forgetting on
  the reader is only half the job: remove the reader from the host's Bluetooth
  settings too, or its stale bond will refuse to re-pair.

### Buttons while Remote is open

| Button | Action |
| --- | --- |
| PageForward, Right | Next |
| PageBack, Left | Previous |
| Confirm | Primary |
| Back, short press | Escape |
| Back, long press | Leave Remote |
| Up / Down | Aux 1 / Aux 2 |
| Power | Unchanged system behavior |

None of these reach the reader while Remote is open. Leaving restores the
previous screen and reading position untouched.

### Profiles

| Profile | Next | Previous | Primary | Escape | Aux 1 | Aux 2 |
| --- | --- | --- | --- | --- | --- | --- |
| Presentation | Right Arrow | Left Arrow | `B` | Escape | Page Down | Page Up |
| Media | Scan Next | Scan Previous | Play/Pause | Escape | Volume Up | Volume Down |
| Navigation | Page Down | Page Up | Enter | Escape | Down Arrow | Up Arrow |

Presentation is verified against Keynote and PowerPoint on macOS only. Anything
else may work.

### Shape of the code

- `src/ble/BleRemoteAction.*` — the v1 whitelist, the profile tables, and the
  translation to HID reports.
- `src/ble/BleRemoteCore.*` — the connection state machine and the bounded
  action and event queues. No NimBLE, Arduino, or FreeRTOS headers, which is
  what makes it testable on the host.
- `src/ble/BleRemote.*` — the NimBLE adapter, and the only file in the app that
  includes a NimBLE header.
- `src/ble/BleRemoteFake.*` — the simulator stand-in. Scripted, not simulated:
  it models the state machine and queue rules but no radio behavior.
- `src/activities/RemoteActivity.*` and
  `src/activities/settings/BluetoothRemoteSettingsActivity.*` — the two screens.

NimBLE callbacks run on the host task and only set atomics or push into a
bounded queue. Every report is written from the activity loop, and the feature
adds no FreeRTOS task of its own and no new global.

### Three rules the Stage 0 spike forced

1. **Nothing is sent before the link is authenticated.** `Connected` and
   `Ready` are distinct states. Reports sent to a connected-but-unauthenticated
   host are silently discarded, which is exactly what happened to six of the
   spike's fourteen reports during first-time pairing.
2. **Callback order is not guaranteed.** On a bonded reconnect the hardware
   delivered `onAuthenticationComplete` *before* `onConnect`. Authentication is
   accepted from any state, and a late connect cannot demote a ready link.
3. **The passkey is generated in `onPassKeyDisplay()`.** Configuring a static
   passkey bypasses that callback entirely, which would leave the screen with
   nothing to show.

### Deliberate deviations from the spec

- **`begin(mode)`, not `begin(profile)`.** A profile decides which action a
  slot sends, which is a UI concern with no bearing on the transport. The
  adapter takes the advertising mode instead and the activity resolves slots.
- **Radio exclusivity is a Wi-Fi check, not a shared guard.** The spec says to
  reuse the "network busy" guard those activities already share. There is no
  such guard: `hasActivityNamed` has exactly one caller, and the network
  activities are mutually exclusive only because each one replaces the
  foreground activity. Rather than invent a second flag, Remote refuses to
  start while Wi-Fi is still up. The reverse direction is already covered,
  because starting any network activity replaces Remote and its `onExit()`
  runs `releaseAll()` and tears the stack down.
- **At most four bonds needs a build flag.** The prebuilt SDK ships
  `CONFIG_BT_NIMBLE_MAX_BONDS=3`. NimBLE-Arduino compiles its own host, so the
  build raises it to 4.

### Where `releaseAll()` runs

`RemoteActivity::onExit()` calls `releaseAll()` and then `end()`, which
releases again before dropping the link. Because every other path out of Remote
goes through the activity stack — Back, sleep, USB Drive, OTA, and any network
activity, all of which replace or pop the foreground activity — `onExit()` is
the single chokepoint, and the host is never left holding a key.

## Open issues

### Unexplained panic on the Stage 0 spike build — not diagnosed

During Stage 0 testing the X4 Pro panicked on its own, with no reset or
unplug, and booted into the crash screen. The reader reported "No reason was
recorded", meaning no software panic message was captured, which points at a
watchdog or brownout reset rather than an assert or a null dereference. The
retained backtrace and registers were written to `/crash_report.txt` on the SD
card and **have not been read**, so the cause is unknown. Nothing below is a
diagnosis.

What is known:

- It happened on the throwaway `x4-pro-blespike` build, not on the shipping
  feature. That build blocks `setup()` for up to three minutes, running eleven
  NimBLE init/deinit cycles and a 120-second advertising window before the main
  loop ever starts. The shipping feature does none of that: it brings the stack
  up once from a foreground activity and drives it from the normal loop.
- The device was booted and exercised repeatedly on that build over roughly an
  hour, including several forced resets over the USB serial line.
- A boot captured immediately afterwards was completely clean, with the probe
  running and reporting its usual figures.

This may well be an artifact of the probe's blocking design rather than
anything in the feature, but that is an assumption and has not been checked.
Read `/crash_report.txt` before trusting the feature on hardware, and treat
hardware acceptance check 2 (200 presses) and check 4 (50 enter/exit cycles) as
the places this would resurface if it is real.

## Hardware acceptance

Spec section 8, on the actual X4 Pro through the pogo USB adapter. Status is
recorded honestly: nothing is marked verified that was not observed.

The Stage 0 spike environment `x4-pro-blespike` has been removed now that its
results are recorded above, so these run against the shipping image:

```bash
pio run -e x4-pro -t upload
```

| # | Check | Status |
| --- | --- | --- |
| 1 | Pair to macOS with passkey; bond survives a reboot | **Verified on the Stage 0 spike**, not yet re-run on the shipping build. Passkey generation moved from a static code to `onPassKeyDisplay()`, so this must be repeated. |
| 2 | 200 presses in Keynote, including rapid bursts and disconnect while held; no stuck or repeated keys | Not yet tested |
| 3 | Leave Remote and confirm the previous book reopens at the same page | Not yet tested |
| 4 | 50 enter/exit cycles; internal heap and largest block before and after, no progressive loss | Not yet tested |
| 5 | Auto-sleep fires with keep-awake off and does not with it on; host sees a clean disconnect either way | Not yet tested |
| 6 | Enter USB Drive after Remote and confirm MSC still enumerates | Not yet tested |
| 7 | Record firmware size, heap numbers, and reconnect time | Firmware size and Stage 0 heap and reconnect numbers recorded above; shipping-build heap not yet measured |

Check 4 is the one that matters most. Stage 0 showed that an advertise-only
cycle returns to baseline exactly, and that a bonded reconnect does too, but
the very first bonding cost a one-time 892 B and 32 KB of contiguity. Fifty
cycles is what proves that stays one-time. If it compounds, the adapter must
keep the stack resident after first use instead of tearing it down per use.

Check 6 matters because Remote and USB Drive both want exclusive ownership of
scarce resources — the radio and internal RAM for one, TinyUSB and the
filesystem for the other. Entering USB Drive replaces the foreground activity,
so `RemoteActivity::onExit()` releases keys and deinitializes NimBLE first, but
that ordering deserves a real check on hardware.

### Reading check 4's numbers

For comparison against the Stage 0 figures above, on a device freshly booted to
Home:

| Phase | Internal free | Internal largest block |
| --- | ---: | ---: |
| Before opening Remote the first time | ~189,700 | ~147,400 |
| With a host connected | ~122,900 | ~81,900 |
| After leaving Remote | ~189,500 | ~114,700 |

The largest-block figure not returning to ~147,400 is expected and one-time.
Internal free dropping below ~189,000 across repeated cycles, or the largest
block falling further on each pass, is the failure this check is looking for.
