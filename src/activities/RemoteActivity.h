#pragma once

#include "AppCapabilities.h"

#if CROSSINK_APP_CAP_BLE_REMOTE

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>

#include "activities/Activity.h"
#include "ble/BleRemoteAdapter.h"

// Foreground-only BLE HID control surface for the X4 Pro. While it is on top,
// the physical buttons drive the paired host and never reach the reader; on
// exit the previous activity and reading position are untouched.
class RemoteActivity final : public Activity {
  using UiApp = freeink::ui::FreeInkApp<16, 4>;

 public:
  RemoteActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool pairNewHost = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Only while a host is actually connected, the toggle is on, and this
  // activity is in the foreground. ActivityManager consults the current
  // activity only, so foreground is implicit.
  bool preventAutoSleep() override;

  // The reader's own menu gestures would be confusing here; this screen owns
  // its input.
  bool allowGlobalHomeGesture() const override { return false; }
  bool allowGlobalHomeSwipeGesture() const override { return false; }

 private:
  static constexpr unsigned long BACK_LONG_PRESS_MS = 600;

  void sendSlot(ble_remote::Slot slot);
  void drainAdapterEvents();
  void refreshTiles();
  const char* statusText() const;
  const char* profileName() const;

  static void remoteScreen(UiApp::ScreenType& screen, void* user);
  static void onTileEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildRemoteScreen(UiApp::ScreenType& screen);

  // Held by value: the adapter object itself is small, and the stack it owns
  // is brought up in onEnter() and released in onExit().
  ble_remote::Adapter remote;
  bool pairNewHost;
  bool startFailed = false;
  bool authRefused = false;
  bool wifiBusy = false;
  bool backLongPressFired = false;

  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  std::atomic<bool> uiReady{false};
  char passkeyText[16] = {};
  // Tiles are laid out three per row: the core clicker row (Previous,
  // Primary, Next) and, when the profile maps them, a second row of Escape
  // and the two aux slots. That is the 1x3 to 2x3 the screen shows.
  static constexpr uint8_t TILE_COLUMNS = 3;
  freeink::ui::TileGridItem tiles[ble_remote::SLOT_COUNT]{};
  uint16_t tileCount = 0;
};

#endif
