#pragma once

#include "AppCapabilities.h"

#if CROSSINK_APP_CAP_BLE_REMOTE

#include "I18nKeys.h"
#include "ble/BleRemoteAction.h"

namespace ble_remote {

// Localized name for one action. Kept beside the action table so the Remote
// screen and the settings screen cannot drift apart.
StrId actionLabel(Action action);

// The Custom picker offers exactly this list, in this order. It is the v1
// whitelist plus the leading "Unset" entry.
extern const Action CUSTOM_CHOICES[];
extern const size_t CUSTOM_CHOICE_COUNT;

}  // namespace ble_remote

#endif
