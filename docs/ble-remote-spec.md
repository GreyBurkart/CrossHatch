# CrossHatch BLE Remote: X4 Pro as a Bluetooth HID Control Surface

Version: 1.0 — standalone feature specification
Date: 2026-09-22
Target: Xteink X4 Pro only; `x4-pro` firmware and `x4-pro-simulator`
Baseline: `GreyBurkart/CrossHatch`, branch `personal`, commit `bbed870c`

**Status:** Specification only. This feature is independent of any companion
GATT transfer protocol, Mac helper, Wi-Fi handoff, or text card. None of those
are prerequisites, and this spec must not introduce them.

## 1. Outcome

In an explicit foreground **Remote** activity, the X4 Pro presents itself to a
paired Mac, iPad, iPhone, Windows PC, or other BLE HID host as a small
programmable remote. Pressing a physical button or a large touch control sends
one standard HID key or consumer-control action to the host. Leaving the
activity ends all Bluetooth behavior.

Direction is X4 Pro to host only. The host never drives CrossHatch.

Primary use: advancing slides or cues in presentation and media software from
the reader while it is already in hand. No host-side software is required.

## 2. Verified baseline

| Fact | Evidence |
| --- | --- |
| The pinned prebuilt S3 SDK already enables the BT controller, the NimBLE host, and the peripheral role. No Bluedroid. | `~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/sdkconfig`: `CONFIG_BT_ENABLED=y`, `CONFIG_BT_NIMBLE_ENABLED=y`, `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y`, `CONFIG_ESP_COEX_ENABLED=y` |
| `lib_ignore = BLE` in the base env only suppresses Arduino's built-in BLE wrapper. NimBLE-Arduino as a `lib_deps` entry does not require an SDK rebuild or any TinyUSB change. | `platformio.ini:107` |
| Flash headroom is tight. The current `x4-pro` image is 6,065,792 bytes against a 6,553,600-byte OTA slot, about 488 KB free. | `.pio/build/x4-pro/firmware.bin`, `partitions.csv` |
| No Bluetooth code exists in `src`, `lib`, `include`, or `freeink-sdk`. | `rg -il bluetooth\|nimble\|esp_bt` returns nothing |
| Logical buttons available to activities: Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward. | `src/MappedInputManager.h:14` |
| Auto-sleep defaults to 10 minutes; 31 means never. | `src/CrossPointSettings.h:529-643` |
| Nearby transfer, Nearby stats sync, the web server, Calibre, OTA, and USB Drive are separate foreground activities that own the radio or USB. | `src/activities/network/` |

## 3. Stage 0 gate: size and lifecycle spike

Before any UI work, land a throwaway branch that adds pinned NimBLE-Arduino to
`x4-pro`, initializes the stack, advertises a minimal HID keyboard, and tears it
down. Record:

- Firmware size delta. If the image no longer fits the OTA slot, stop and
  report. Shrinking the SPIFFS slice is a separate decision because it forces a
  full reflash for every X4 Pro user. Do not silently change `partitions.csv`.
- Free internal heap and largest allocatable internal block before init, after
  init, after deinit, and after ten init/deinit cycles. PSRAM is not a
  substitute; NimBLE task stacks and controller buffers are internal RAM.
- Whether `NimBLEDevice::deinit(true)` returns memory and allows a later
  re-init on this SDK. If controller memory release is one-way, document it
  and keep the stack resident after first use instead of pretending to free it.
- A passkey pairing to macOS and a successful arrow-key delivery to Keynote or
  a text field.

Exit gate: numbers recorded in `docs/ble-remote.md`, image fits, re-init
works or the one-way case is documented.

## 4. Architecture

- **One adapter class** in `src/ble/` (new), for example `BleRemote`, wrapping
  NimBLE-Arduino behind a small interface: `begin(profile)`, `end()`,
  `sendAction(action)`, `releaseAll()`, `state()`, `forgetBondedHost()`. App
  code never includes NimBLE headers outside this class.
- **Capability gate:** new `CROSSINK_APP_CAP_BLE_REMOTE` (0/1) defined per
  environment in `platformio.ini`, validated in `include/AppCapabilities.h`
  like the existing touch and USB Drive caps. Only `x4-pro`, `x4-pro-debug`,
  and `x4-pro-simulator` set it to 1. C3 and Sticky builds compile nothing new.
- **Simulator:** a deterministic fake behind the same interface that logs
  actions and scripts connect, disconnect, and pairing events. It does not
  simulate radio behavior.
- **Callback discipline:** NimBLE callbacks run on the host task. They may set
  atomic state and push bounded events into a fixed-capacity queue. They must
  not render, touch the activity stack, or block on SD. The activity drains
  the queue in its normal loop.
- **No FreeRTOS task of our own.** NimBLE owns its host task. The activity
  sends reports from the main loop.
- **No new global.** State lives in the activity plus one settings block.

## 5. Remote activity

New `src/activities/RemoteActivity.{h,cpp}` (foreground only, entered from
Settings and optionally from Quick Actions using the existing shortcut system).

### Screen

Static e-ink layout: host name or "Not connected", connection state, active
profile name, and four to six large labeled touch targets. Redraw only on state
or profile change, never per press. A press may flash a brief inverted label
if the theme already supports cheap partial refresh; otherwise no feedback
beyond the host reacting.

### Physical button map (required, not optional)

| Logical button | Action |
| --- | --- |
| PageForward, Right | Next |
| PageBack, Left | Previous |
| Confirm | Primary (Play/Pause or Start) |
| Back (short) | Escape |
| Back (long) | Leave Remote activity |
| Up / Down | Aux 1 / Aux 2 |
| Power | Unchanged system behavior |

In Remote mode these buttons do not act on the reader. On exit, the prior
activity and reading position are untouched.

### Profiles

Stored in `SETTINGS` as one small struct: active profile id, and for Custom,
one action id per slot. No macro editor, no text.

| Profile | Next | Previous | Primary | Escape | Aux 1 | Aux 2 |
| --- | --- | --- | --- | --- | --- | --- |
| Presentation | Right Arrow | Left Arrow | Keyboard `B` (blank) | Escape | Page Down | Page Up |
| Media | Consumer Scan Next | Consumer Scan Previous | Consumer Play/Pause | Escape | Volume Up | Volume Down |
| Navigation | Page Down | Page Up | Enter | Escape | Down Arrow | Up Arrow |
| Custom | any from whitelist | | | | | |

Whitelist for Custom: arrows, Page Up/Down, Home, End, Enter, Escape, Space,
Tab, `B`, `W`, F5, Shift+F5, consumer Play/Pause, Scan Next, Scan Previous,
Stop, Volume Up, Volume Down, Mute. Nothing else in v1. No modifier
combinations beyond Shift+F5.

Host-specific claims are not made. The Presentation profile is verified
against Keynote and PowerPoint on macOS only; anything else is "may work".

### HID behavior

- Report map: one keyboard report (8 bytes: modifiers, reserved, 6 keys) and
  one consumer-control report (16-bit usage). Boot protocol not required.
- One physical press produces exactly one press report followed by one release
  report. Auto-repeat is off. Holding a button does nothing further.
- `releaseAll()` sends an empty keyboard report and a zero consumer report. It
  runs on activity exit, disconnect, before sleep, before USB Drive/OTA, and on
  any mode change. The host must never be left with a stuck key.
- Queue capacity: 8 pending actions. If the host is not connected, presses
  are dropped, not queued for later replay.

## 6. Pairing, trust, and lifecycle

- **Pairing method is pinned:** the X4 Pro declares I/O capability
  DisplayOnly, bonding on, MITM on, secure connections on. The reader shows a
  six-digit passkey; the host types it. Just Works is refused for this
  service. This gives authenticated pairing on macOS, iOS, iPadOS, and Windows.
- **Address:** use a static random address persisted in NVS so the host's
  bond remains valid across reboots.
- **Bonds:** at most 4 stored hosts. Bonds live in the 20 KB NVS partition.
  Forget on the reader deletes the NimBLE bond entry. The UI must tell the
  user to also forget the device on the host, because a stale host bond will
  refuse to re-pair.
- **Pair New Host** is an explicit menu action that advertises as connectable
  and discoverable for 60 seconds. Ordinary Remote entry advertises only to
  bonded hosts (filter accept list) for 30 seconds, then shows "Not connected"
  with a Retry control.
- **Sleep:** entering Remote does not change the sleep policy. Button presses
  count as user interaction and reset the timer as they do elsewhere.
  Advertising or a connected idle link does not. Add one setting, off by
  default: "Keep awake while Remote is connected". When on, the sleep timer is
  suspended only while a host is connected and the Remote activity is in the
  foreground. Leaving the activity restores normal behavior immediately.
- **Radio exclusivity:** Remote refuses to start while Wi-Fi, Nearby transfer,
  Nearby stats sync, web server, Calibre, OTA, or USB Drive is active, and
  those refuse to start while Remote is active. Reuse whatever "network busy"
  guard those activities already share rather than adding a second flag.
- **Deep sleep:** the radio is off in deep sleep. No wake-over-BLE. The user
  wakes the reader and reopens Remote.

Deferred, not planned: mouse/trackpad, text entry, macros, MIDI, background
always-on mode, per-app presets, concurrent BLE connections, host-to-reader
feedback, any reuse of this bond for file transfer.

## 7. Settings UI

Under Settings, a "Bluetooth Remote" entry (capability-gated) with: Open
Remote, Profile, Custom slot mapping, Keep awake toggle, Paired hosts list
with Forget, Pair New Host. All strings through `tr(STR_*)` and the i18n YAML.
Existing list components only.

## 8. Testing

Native tests under `test/ble_remote/` covering the pure logic with the fake
adapter: profile to report translation, press/release pairing, queue
saturation drops, `releaseAll()` on every exit path, late events after the
activity exits, custom-slot validation against the whitelist, settings
round-trip.

Build gate: `pio run -e x4-pro`, `pio run -e x4-pro-simulator`, and as
regression `pio run -e default` and `pio run -e sticky` must still build with
no new symbols. `pio check -e default` stays clean.

Hardware acceptance on the actual X4 Pro through the pogo USB adapter:

- Pair to macOS with passkey; confirm the bond survives a reboot.
- 200 presses in Keynote including rapid bursts and disconnect while held; no
  stuck or repeated keys.
- Leave Remote and confirm the previous book reopens at the same page.
- 50 enter/exit cycles; report internal heap and largest block before and
  after with no progressive loss.
- Auto-sleep fires with keep-awake off and does not with it on; the host sees
  a clean disconnect either way.
- Enter USB Drive after Remote and confirm MSC still enumerates.
- Record firmware size, heap numbers, and reconnect time in
  `docs/ble-remote.md`.

## 9. Repository placement

- `src/ble/BleRemote.{h,cpp}` and `src/ble/BleRemoteFake.{h,cpp}`
- `src/activities/RemoteActivity.{h,cpp}`
- `src/activities/settings/BluetoothRemoteSettingsActivity.{h,cpp}`
- `include/AppCapabilities.h` (new cap), `platformio.ini` (lib_deps and cap
  flag on the three x4-pro environments only)
- `lib/I18n/translations/*.yaml` then regenerate; never edit generated files
- `docs/ble-remote.md` user and measurement doc; `CHANGELOG.md` Added entry;
  a W-number in `docs/wishlist.md` that references this doc and stays clear of
  W7's deferred import pipeline.

This document authorizes design work and the Stage 0 spike on a branch. It
does not authorize commits to `personal`, partition changes, or SDK submodule
changes.
