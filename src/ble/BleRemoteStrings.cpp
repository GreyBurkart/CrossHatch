#include "ble/BleRemoteStrings.h"

#if CROSSINK_APP_CAP_BLE_REMOTE

namespace ble_remote {

StrId actionLabel(Action action) {
  switch (action) {
    case Action::ArrowRight:
      return StrId::STR_BLE_REMOTE_ACTION_ARROW_RIGHT;
    case Action::ArrowLeft:
      return StrId::STR_BLE_REMOTE_ACTION_ARROW_LEFT;
    case Action::ArrowUp:
      return StrId::STR_BLE_REMOTE_ACTION_ARROW_UP;
    case Action::ArrowDown:
      return StrId::STR_BLE_REMOTE_ACTION_ARROW_DOWN;
    case Action::PageUp:
      return StrId::STR_BLE_REMOTE_ACTION_PAGE_UP;
    case Action::PageDown:
      return StrId::STR_BLE_REMOTE_ACTION_PAGE_DOWN;
    case Action::Home:
      return StrId::STR_BLE_REMOTE_ACTION_HOME;
    case Action::End:
      return StrId::STR_BLE_REMOTE_ACTION_END;
    case Action::Enter:
      return StrId::STR_BLE_REMOTE_ACTION_ENTER;
    case Action::Escape:
      return StrId::STR_BLE_REMOTE_ACTION_ESCAPE;
    case Action::Space:
      return StrId::STR_BLE_REMOTE_ACTION_SPACE;
    case Action::Tab:
      return StrId::STR_BLE_REMOTE_ACTION_TAB;
    case Action::KeyB:
      return StrId::STR_BLE_REMOTE_ACTION_KEY_B;
    case Action::KeyW:
      return StrId::STR_BLE_REMOTE_ACTION_KEY_W;
    case Action::F5:
      return StrId::STR_BLE_REMOTE_ACTION_F5;
    case Action::ShiftF5:
      return StrId::STR_BLE_REMOTE_ACTION_SHIFT_F5;
    case Action::MediaPlayPause:
      return StrId::STR_BLE_REMOTE_ACTION_PLAY_PAUSE;
    case Action::MediaScanNext:
      return StrId::STR_BLE_REMOTE_ACTION_SCAN_NEXT;
    case Action::MediaScanPrevious:
      return StrId::STR_BLE_REMOTE_ACTION_SCAN_PREVIOUS;
    case Action::MediaStop:
      return StrId::STR_BLE_REMOTE_ACTION_STOP;
    case Action::VolumeUp:
      return StrId::STR_BLE_REMOTE_ACTION_VOLUME_UP;
    case Action::VolumeDown:
      return StrId::STR_BLE_REMOTE_ACTION_VOLUME_DOWN;
    case Action::Mute:
      return StrId::STR_BLE_REMOTE_ACTION_MUTE;
    case Action::None:
    case Action::ACTION_COUNT:
      break;
  }
  return StrId::STR_BLE_REMOTE_ACTION_NONE;
}

// static const keeps this table in flash.
const Action CUSTOM_CHOICES[] = {
    Action::None,       Action::ArrowRight,     Action::ArrowLeft,      Action::ArrowUp,
    Action::ArrowDown,  Action::PageUp,         Action::PageDown,       Action::Home,
    Action::End,        Action::Enter,          Action::Escape,         Action::Space,
    Action::Tab,        Action::KeyB,           Action::KeyW,           Action::F5,
    Action::ShiftF5,    Action::MediaPlayPause, Action::MediaScanNext,  Action::MediaScanPrevious,
    Action::MediaStop,  Action::VolumeUp,       Action::VolumeDown,     Action::Mute,
};

const size_t CUSTOM_CHOICE_COUNT = sizeof(CUSTOM_CHOICES) / sizeof(CUSTOM_CHOICES[0]);

}  // namespace ble_remote

#endif
