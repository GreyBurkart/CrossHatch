#pragma once

// The app deliberately has a build-time touch capability, separate from a
// runtime probe. Button-only images must not retain the touch UI merely because
// the shared source tree also builds for Sticky.
#ifndef CROSSINK_APP_CAP_TOUCH
#error "Define CROSSINK_APP_CAP_TOUCH as 0 or 1 in the PlatformIO environment"
#endif

#if CROSSINK_APP_CAP_TOUCH != 0 && CROSSINK_APP_CAP_TOUCH != 1
#error "CROSSINK_APP_CAP_TOUCH must be 0 or 1"
#endif

#ifndef CROSSINK_APP_CAP_USB_DRIVE
#error "Define CROSSINK_APP_CAP_USB_DRIVE as 0 or 1 in the PlatformIO environment"
#endif

#if CROSSINK_APP_CAP_USB_DRIVE != 0 && CROSSINK_APP_CAP_USB_DRIVE != 1
#error "CROSSINK_APP_CAP_USB_DRIVE must be 0 or 1"
#endif

// The Bluetooth Remote is an X4 Pro-only control surface. Keeping it a
// build-time capability rather than a runtime probe means the C3 and Sticky
// images link no NimBLE code at all, which is the whole reason the flash and
// internal-RAM cost is affordable. See docs/ble-remote.md.
#ifndef CROSSINK_APP_CAP_BLE_REMOTE
#error "Define CROSSINK_APP_CAP_BLE_REMOTE as 0 or 1 in the PlatformIO environment"
#endif

#if CROSSINK_APP_CAP_BLE_REMOTE != 0 && CROSSINK_APP_CAP_BLE_REMOTE != 1
#error "CROSSINK_APP_CAP_BLE_REMOTE must be 0 or 1"
#endif

// Native simulator BoardConfig intentionally exposes only simulated runtime
// profiles, so keep this firmware-image identity available at the app layer.
#if defined(FREEINK_DEVICE_X4CLASSIC) && FREEINK_DEVICE_X4CLASSIC
#define CROSSINK_APP_DEVICE_X4CLASSIC 1
#else
#define CROSSINK_APP_DEVICE_X4CLASSIC 0
#endif

// Native simulator BoardConfig deliberately has no FREEINK_CAP_TOUCH macro.
// Firmware builds must keep the app and SDK capability selections in lockstep.
#if !defined(SIMULATOR)
#include <BoardConfig.h>
#if CROSSINK_APP_CAP_TOUCH != FREEINK_CAP_TOUCH
#error "CROSSINK_APP_CAP_TOUCH must match FREEINK_CAP_TOUCH"
#endif
#if CROSSINK_APP_CAP_USB_DRIVE != FREEINK_CAP_USB_MSC
#error "CROSSINK_APP_CAP_USB_DRIVE must match FREEINK_CAP_USB_MSC"
#endif
// There is no SDK capability macro for Bluetooth, so anchor the app capability
// to the one board whose prebuilt SDK ships the BT controller and the NimBLE
// peripheral role. This is what keeps a stray flag from pulling NimBLE into a
// C3 image, where it would not fit.
#if CROSSINK_APP_CAP_BLE_REMOTE && !(defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO)
#error "CROSSINK_APP_CAP_BLE_REMOTE is only supported on the X4 Pro firmware profile"
#endif
#endif
