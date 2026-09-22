#include "ble/BleRemoteAction.h"

namespace ble_remote {
namespace {

// HID keyboard usage ids (usage page 0x07) and modifier bits.
constexpr uint8_t KEY_B = 0x05;
constexpr uint8_t KEY_W = 0x1A;
constexpr uint8_t KEY_ENTER = 0x28;
constexpr uint8_t KEY_ESCAPE = 0x29;
constexpr uint8_t KEY_TAB = 0x2B;
constexpr uint8_t KEY_SPACE = 0x2C;
constexpr uint8_t KEY_F5 = 0x3E;
constexpr uint8_t KEY_HOME = 0x4A;
constexpr uint8_t KEY_PAGE_UP = 0x4B;
constexpr uint8_t KEY_END = 0x4D;
constexpr uint8_t KEY_PAGE_DOWN = 0x4E;
constexpr uint8_t KEY_ARROW_RIGHT = 0x4F;
constexpr uint8_t KEY_ARROW_LEFT = 0x50;
constexpr uint8_t KEY_ARROW_DOWN = 0x51;
constexpr uint8_t KEY_ARROW_UP = 0x52;
constexpr uint8_t MOD_LEFT_SHIFT = 0x02;

// Consumer page (0x0C) usage ids.
constexpr uint16_t USAGE_SCAN_NEXT = 0x00B5;
constexpr uint16_t USAGE_SCAN_PREVIOUS = 0x00B6;
constexpr uint16_t USAGE_STOP = 0x00B7;
constexpr uint16_t USAGE_PLAY_PAUSE = 0x00CD;
constexpr uint16_t USAGE_MUTE = 0x00E2;
constexpr uint16_t USAGE_VOLUME_UP = 0x00E9;
constexpr uint16_t USAGE_VOLUME_DOWN = 0x00EA;

constexpr Report key(uint8_t keycode, uint8_t modifiers = 0) {
  return Report{ReportKind::Keyboard, modifiers, keycode, 0};
}

constexpr Report consumer(uint16_t usage) { return Report{ReportKind::Consumer, 0, 0, usage}; }

// Slot order matches ble_remote::Slot. static const keeps these in flash.
struct ProfileRow {
  Action slots[SLOT_COUNT];
};

const ProfileRow PROFILE_TABLE[PROFILE_COUNT] = {
    // Presentation: Next, Previous, Primary, Escape, Aux1, Aux2
    {{Action::ArrowRight, Action::ArrowLeft, Action::KeyB, Action::Escape, Action::PageDown, Action::PageUp}},
    // Media
    {{Action::MediaScanNext, Action::MediaScanPrevious, Action::MediaPlayPause, Action::Escape, Action::VolumeUp,
      Action::VolumeDown}},
    // Navigation
    {{Action::PageDown, Action::PageUp, Action::Enter, Action::Escape, Action::ArrowDown, Action::ArrowUp}},
    // Custom is resolved from settings, never from this table.
    {{Action::None, Action::None, Action::None, Action::None, Action::None, Action::None}},
};

}  // namespace

Report reportFor(Action action) {
  switch (action) {
    case Action::ArrowRight:
      return key(KEY_ARROW_RIGHT);
    case Action::ArrowLeft:
      return key(KEY_ARROW_LEFT);
    case Action::ArrowUp:
      return key(KEY_ARROW_UP);
    case Action::ArrowDown:
      return key(KEY_ARROW_DOWN);
    case Action::PageUp:
      return key(KEY_PAGE_UP);
    case Action::PageDown:
      return key(KEY_PAGE_DOWN);
    case Action::Home:
      return key(KEY_HOME);
    case Action::End:
      return key(KEY_END);
    case Action::Enter:
      return key(KEY_ENTER);
    case Action::Escape:
      return key(KEY_ESCAPE);
    case Action::Space:
      return key(KEY_SPACE);
    case Action::Tab:
      return key(KEY_TAB);
    case Action::KeyB:
      return key(KEY_B);
    case Action::KeyW:
      return key(KEY_W);
    case Action::F5:
      return key(KEY_F5);
    case Action::ShiftF5:
      return key(KEY_F5, MOD_LEFT_SHIFT);
    case Action::MediaPlayPause:
      return consumer(USAGE_PLAY_PAUSE);
    case Action::MediaScanNext:
      return consumer(USAGE_SCAN_NEXT);
    case Action::MediaScanPrevious:
      return consumer(USAGE_SCAN_PREVIOUS);
    case Action::MediaStop:
      return consumer(USAGE_STOP);
    case Action::VolumeUp:
      return consumer(USAGE_VOLUME_UP);
    case Action::VolumeDown:
      return consumer(USAGE_VOLUME_DOWN);
    case Action::Mute:
      return consumer(USAGE_MUTE);
    case Action::None:
    case Action::ACTION_COUNT:
      break;
  }
  return Report{};
}

Action resolveSlot(Profile profile, Slot slot, const uint8_t* customSlots, size_t customSlotCount) {
  const auto slotIndex = static_cast<size_t>(slot);
  if (slotIndex >= SLOT_COUNT) {
    return Action::None;
  }
  if (profile != Profile::Custom) {
    return builtInAction(profile, slot);
  }
  if (customSlots == nullptr || slotIndex >= customSlotCount) {
    return Action::None;
  }
  const uint8_t raw = customSlots[slotIndex];
  return isValidAction(raw) ? static_cast<Action>(raw) : Action::None;
}

Action builtInAction(Profile profile, Slot slot) {
  const auto profileIndex = static_cast<size_t>(profile);
  const auto slotIndex = static_cast<size_t>(slot);
  if (profileIndex >= PROFILE_COUNT || slotIndex >= SLOT_COUNT) {
    return Action::None;
  }
  return PROFILE_TABLE[profileIndex].slots[slotIndex];
}

}  // namespace ble_remote
