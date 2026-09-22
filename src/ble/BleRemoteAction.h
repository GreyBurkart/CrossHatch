#pragma once

#include <cstddef>
#include <cstdint>

// Pure, hardware-free description of what the Bluetooth Remote can send.
// Nothing here includes NimBLE or Arduino headers, so the native tests under
// test/ble_remote/ compile it directly. See docs/ble-remote.md.
namespace ble_remote {

// The six things a press can mean. Slots are fixed; a profile decides which
// action each one sends.
enum class Slot : uint8_t { Next, Previous, Primary, Escape, Aux1, Aux2 };
inline constexpr size_t SLOT_COUNT = 6;

enum class Profile : uint8_t { Presentation, Media, Navigation, Custom };
inline constexpr size_t PROFILE_COUNT = 4;

// The v1 whitelist, and nothing else. Custom slots validate against this, so
// an out-of-range or removed id can never reach the HID layer. Values are
// stable because they are persisted in settings; append only.
enum class Action : uint8_t {
  None = 0,
  ArrowRight,
  ArrowLeft,
  ArrowUp,
  ArrowDown,
  PageUp,
  PageDown,
  Home,
  End,
  Enter,
  Escape,
  Space,
  Tab,
  KeyB,
  KeyW,
  F5,
  ShiftF5,
  MediaPlayPause,
  MediaScanNext,
  MediaScanPrevious,
  MediaStop,
  VolumeUp,
  VolumeDown,
  Mute,
  ACTION_COUNT
};

// How one action is expressed on the wire. Exactly one report kind per action;
// the spec allows no modifier combination beyond Shift+F5.
enum class ReportKind : uint8_t { None, Keyboard, Consumer };

struct Report {
  ReportKind kind = ReportKind::None;
  uint8_t modifiers = 0;  // HID modifier bitmap, keyboard reports only
  uint8_t keycode = 0;    // HID usage id, keyboard reports only
  uint16_t usage = 0;     // consumer usage, consumer reports only

  constexpr bool isEmpty() const { return kind == ReportKind::None; }
};

// True for ids inside the whitelist. None is a valid stored value (an unset
// Custom slot) but never produces a report.
constexpr bool isValidAction(uint8_t raw) { return raw < static_cast<uint8_t>(Action::ACTION_COUNT); }

// Translates an action to its single press report. An unknown or None action
// yields an empty report, which callers must not send.
Report reportFor(Action action);

// The built-in profile tables. Custom reads from settings instead.
Action builtInAction(Profile profile, Slot slot);

// Resolves one slot for the active profile. customSlots is consulted only for
// Profile::Custom, and an entry outside the whitelist resolves to None rather
// than reaching the HID layer. Passed explicitly rather than read from SETTINGS
// so this stays testable without the firmware settings singleton.
Action resolveSlot(Profile profile, Slot slot, const uint8_t* customSlots, size_t customSlotCount);

// True when raw names a profile. Used to validate persisted settings.
constexpr bool isValidProfile(uint8_t raw) { return raw < PROFILE_COUNT; }

}  // namespace ble_remote
