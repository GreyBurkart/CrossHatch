#pragma once

#include "AppCapabilities.h"

#if CROSSINK_APP_CAP_BLE_REMOTE

#if defined(SIMULATOR)
#include "ble/BleRemoteFake.h"
#else
#include "ble/BleRemote.h"
#endif

namespace ble_remote {

// One name for whichever transport this image was built with, so activities
// never spell out either concrete type.
#if defined(SIMULATOR)
using Adapter = BleRemoteFake;
#else
using Adapter = BleRemote;
#endif

}  // namespace ble_remote

#endif
