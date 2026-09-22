#pragma once

#include "AppCapabilities.h"

#if CROSSINK_APP_CAP_BLE_REMOTE

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>

#include "I18nKeys.h"
#include "activities/Activity.h"
#include "ble/BleRemoteAction.h"
#include "ble/BleRemoteTypes.h"
#include "util/ButtonNavigator.h"

// Settings > Bluetooth Remote. Opens the Remote screen, picks the profile,
// edits Custom slots, toggles keep-awake, and manages bonded hosts.
class BluetoothRemoteSettingsActivity final : public Activity {
  using UiApp = freeink::ui::FreeInkApp<16, 4>;

 public:
  BluetoothRemoteSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Which list the screen is showing. Kept in one activity so a Custom slot
  // edit does not need a third screen.
  enum class View : uint8_t { Root, CustomSlots, ActionPicker, PairedHosts };

  static constexpr size_t MAX_ROWS = 24;

  void openView(View view);
  void activateRootRow(int index);
  void activateCustomSlotRow(int index);
  void activateActionPickerRow(int index);
  void activatePairedHostRow(int index);
  void refreshBondedHosts();
  void cycleProfile();
  void toggleKeepAwake();
  void rebuildRows();
  size_t rootRowCount() const;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
  StrId title() const;

  View view = View::Root;
  int selectedIndex = 0;
  int editingSlot = 0;
  bool dirty = false;
  size_t bondedHosts = 0;
  // Reading the bond store costs a full stack bring-up, so it is deferred until
  // the paired-host list is opened rather than paid on every screen entry.
  bool bondedHostsKnown = false;

  ButtonNavigator buttonNavigator;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;
  size_t rowCount = 0;
  freeink::ui::ListItem rowItems[MAX_ROWS]{};
  // Row labels that are not static translated strings need somewhere stable to
  // live for the duration of a render.
  char hostLabels[ble_remote::MAX_BONDED_HOSTS][24]{};
};

#endif
