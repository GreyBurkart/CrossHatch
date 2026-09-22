#include "activities/settings/BluetoothRemoteSettingsActivity.h"

#if CROSSINK_APP_CAP_BLE_REMOTE

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/RemoteActivity.h"
#include "ble/BleRemoteAdapter.h"
#include "ble/BleRemoteStrings.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;
using ble_remote::Action;
using ble_remote::Slot;

namespace {

constexpr char TAG[] = "BLEREMSET";
constexpr fui::ActionId ACTION_ROW = 1;

// Root rows, in order.
enum RootRow : int {
  ROOT_OPEN_REMOTE = 0,
  ROOT_PROFILE,
  ROOT_CUSTOM_SLOTS,
  ROOT_KEEP_AWAKE,
  ROOT_PAIR_NEW,
  ROOT_PAIRED_HOSTS,
  ROOT_ROW_COUNT,
};

constexpr Slot SLOT_ORDER[ble_remote::SLOT_COUNT] = {Slot::Next,   Slot::Previous, Slot::Primary,
                                                     Slot::Escape, Slot::Aux1,     Slot::Aux2};

StrId slotLabel(Slot slot) {
  switch (slot) {
    case Slot::Next:
      return StrId::STR_BLE_REMOTE_SLOT_NEXT;
    case Slot::Previous:
      return StrId::STR_BLE_REMOTE_SLOT_PREVIOUS;
    case Slot::Primary:
      return StrId::STR_BLE_REMOTE_SLOT_PRIMARY;
    case Slot::Escape:
      return StrId::STR_BLE_REMOTE_SLOT_ESCAPE;
    case Slot::Aux1:
      return StrId::STR_BLE_REMOTE_SLOT_AUX1;
    case Slot::Aux2:
      return StrId::STR_BLE_REMOTE_SLOT_AUX2;
  }
  return StrId::STR_BLE_REMOTE_SLOT_NEXT;
}

StrId profileLabel(ble_remote::Profile profile) {
  switch (profile) {
    case ble_remote::Profile::Media:
      return StrId::STR_BLE_REMOTE_PROFILE_MEDIA;
    case ble_remote::Profile::Navigation:
      return StrId::STR_BLE_REMOTE_PROFILE_NAVIGATION;
    case ble_remote::Profile::Custom:
      return StrId::STR_BLE_REMOTE_PROFILE_CUSTOM;
    case ble_remote::Profile::Presentation:
      break;
  }
  return StrId::STR_BLE_REMOTE_PROFILE_PRESENTATION;
}

ble_remote::Profile activeProfile() { return static_cast<ble_remote::Profile>(SETTINGS.bleRemoteProfile); }

// Counting bonds needs the stack briefly, and bringing it up costs about 67 KB
// of internal RAM. That is far too much to spend just to open this screen, so
// callers must only do it when the paired-host list is actually shown.
size_t readBondedHostCount(ble_remote::Adapter& adapter) {
  if (!adapter.begin(ble_remote::AdvertiseMode::BondedOnly)) {
    LOG_ERR(TAG, "could not start adapter to read bonds");
    return 0;
  }
  const size_t count = adapter.bondedHostCount();
  adapter.end();
  return count;
}

}  // namespace

BluetoothRemoteSettingsActivity::BluetoothRemoteSettingsActivity(GfxRenderer& renderer,
                                                                 MappedInputManager& mappedInput)
    : Activity("BluetoothRemoteSettings", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void BluetoothRemoteSettingsActivity::onEnter() {
  Activity::onEnter();
  view = View::Root;
  selectedIndex = 0;
  topIndex = 0;
  visibleRows = 1;
  dirty = false;
  uiReady = false;
  bondedHosts = 0;
  bondedHostsKnown = false;
  rebuildRows();
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &BluetoothRemoteSettingsActivity::onRowEvent, this);
  app.setScreen(&BluetoothRemoteSettingsActivity::listScreen, this);
  requestUpdate();
}

void BluetoothRemoteSettingsActivity::onExit() {
  if (dirty) {
    SETTINGS.saveToFile();
  }
  Activity::onExit();
}

size_t BluetoothRemoteSettingsActivity::rootRowCount() const { return ROOT_ROW_COUNT; }

StrId BluetoothRemoteSettingsActivity::title() const {
  switch (view) {
    case View::CustomSlots:
      return StrId::STR_BLE_REMOTE_CUSTOM_SLOTS;
    case View::ActionPicker:
      return slotLabel(SLOT_ORDER[editingSlot]);
    case View::PairedHosts:
      return StrId::STR_BLE_REMOTE_PAIRED_HOSTS;
    case View::Root:
      break;
  }
  return StrId::STR_BLE_REMOTE;
}

void BluetoothRemoteSettingsActivity::openView(View next) {
  view = next;
  selectedIndex = 0;
  topIndex = 0;
  rebuildRows();
  requestUpdate();
}

void BluetoothRemoteSettingsActivity::rebuildRows() {
  rowCount = 0;
  switch (view) {
    case View::Root: {
      rowItems[ROOT_OPEN_REMOTE].label = tr(STR_BLE_REMOTE_OPEN);
      rowItems[ROOT_OPEN_REMOTE].value = nullptr;
      rowItems[ROOT_PROFILE].label = tr(STR_BLE_REMOTE_PROFILE);
      rowItems[ROOT_PROFILE].value = I18N.get(profileLabel(activeProfile()));
      rowItems[ROOT_CUSTOM_SLOTS].label = tr(STR_BLE_REMOTE_CUSTOM_SLOTS);
      rowItems[ROOT_CUSTOM_SLOTS].value = nullptr;
      rowItems[ROOT_KEEP_AWAKE].label = tr(STR_BLE_REMOTE_KEEP_AWAKE);
      rowItems[ROOT_KEEP_AWAKE].value = SETTINGS.bleRemoteKeepAwake ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
      rowItems[ROOT_PAIR_NEW].label = tr(STR_BLE_REMOTE_PAIR_NEW);
      rowItems[ROOT_PAIR_NEW].value = nullptr;
      rowItems[ROOT_PAIRED_HOSTS].label = tr(STR_BLE_REMOTE_PAIRED_HOSTS);
      rowItems[ROOT_PAIRED_HOSTS].value = nullptr;
      for (int i = 0; i < ROOT_ROW_COUNT; ++i) {
        rowItems[i].actionValue = static_cast<int16_t>(i);
      }
      rowCount = ROOT_ROW_COUNT;
      break;
    }
    case View::CustomSlots: {
      for (size_t i = 0; i < ble_remote::SLOT_COUNT; ++i) {
        const uint8_t raw = SETTINGS.bleRemoteCustomSlots[i];
        const Action action = ble_remote::isValidAction(raw) ? static_cast<Action>(raw) : Action::None;
        rowItems[i].label = I18N.get(slotLabel(SLOT_ORDER[i]));
        rowItems[i].value = I18N.get(ble_remote::actionLabel(action));
        rowItems[i].actionValue = static_cast<int16_t>(i);
      }
      rowCount = ble_remote::SLOT_COUNT;
      break;
    }
    case View::ActionPicker: {
      const size_t count =
          ble_remote::CUSTOM_CHOICE_COUNT < MAX_ROWS ? ble_remote::CUSTOM_CHOICE_COUNT : MAX_ROWS;
      const uint8_t current = SETTINGS.bleRemoteCustomSlots[editingSlot];
      for (size_t i = 0; i < count; ++i) {
        rowItems[i].label = I18N.get(ble_remote::actionLabel(ble_remote::CUSTOM_CHOICES[i]));
        rowItems[i].value =
            static_cast<uint8_t>(ble_remote::CUSTOM_CHOICES[i]) == current ? tr(STR_STATE_ON) : nullptr;
        rowItems[i].actionValue = static_cast<int16_t>(i);
      }
      rowCount = count;
      break;
    }
    case View::PairedHosts: {
      if (bondedHosts == 0) {
        rowItems[0].label = tr(STR_BLE_REMOTE_NO_PAIRED_HOSTS);
        rowItems[0].value = nullptr;
        rowItems[0].actionValue = -1;
        rowCount = 1;
        break;
      }
      const size_t count = bondedHosts < ble_remote::MAX_BONDED_HOSTS ? bondedHosts : ble_remote::MAX_BONDED_HOSTS;
      for (size_t i = 0; i < count; ++i) {
        snprintf(hostLabels[i], sizeof(hostLabels[i]), "%s %u", tr(STR_BLE_REMOTE_PAIRED_HOSTS),
                 static_cast<unsigned>(i + 1));
        rowItems[i].label = hostLabels[i];
        rowItems[i].value = tr(STR_BLE_REMOTE_FORGET);
        rowItems[i].actionValue = static_cast<int16_t>(i);
      }
      rowCount = count;
      break;
    }
  }
  if (selectedIndex >= static_cast<int>(rowCount)) {
    selectedIndex = rowCount > 0 ? static_cast<int>(rowCount) - 1 : 0;
  }
}

void BluetoothRemoteSettingsActivity::refreshBondedHosts() {
  ble_remote::Adapter adapter;
  bondedHosts = readBondedHostCount(adapter);
  bondedHostsKnown = true;
}

void BluetoothRemoteSettingsActivity::cycleProfile() {
  const auto next = static_cast<uint8_t>((SETTINGS.bleRemoteProfile + 1) % ble_remote::PROFILE_COUNT);
  SETTINGS.bleRemoteProfile = next;
  dirty = true;
  rebuildRows();
  app.clearTapFlash();
  requestUpdate();
}

void BluetoothRemoteSettingsActivity::toggleKeepAwake() {
  SETTINGS.bleRemoteKeepAwake = SETTINGS.bleRemoteKeepAwake ? 0 : 1;
  dirty = true;
  rebuildRows();
  app.clearTapFlash();
  requestUpdate();
}

void BluetoothRemoteSettingsActivity::activateRootRow(int index) {
  switch (index) {
    case ROOT_OPEN_REMOTE:
      startActivityForResult(std::make_unique<RemoteActivity>(renderer, mappedInput, false),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
    case ROOT_PROFILE:
      cycleProfile();
      break;
    case ROOT_CUSTOM_SLOTS:
      openView(View::CustomSlots);
      break;
    case ROOT_KEEP_AWAKE:
      toggleKeepAwake();
      break;
    case ROOT_PAIR_NEW:
      startActivityForResult(std::make_unique<RemoteActivity>(renderer, mappedInput, true),
                             [this](const ActivityResult&) {
                               // A successful pairing changes the bond count,
                               // but only refresh it if it was already known.
                               if (bondedHostsKnown) {
                                 refreshBondedHosts();
                               }
                               rebuildRows();
                               requestUpdate();
                             });
      break;
    case ROOT_PAIRED_HOSTS:
      refreshBondedHosts();
      openView(View::PairedHosts);
      break;
    default:
      break;
  }
}

void BluetoothRemoteSettingsActivity::activateCustomSlotRow(int index) {
  if (index < 0 || index >= static_cast<int>(ble_remote::SLOT_COUNT)) {
    return;
  }
  editingSlot = index;
  openView(View::ActionPicker);
}

void BluetoothRemoteSettingsActivity::activateActionPickerRow(int index) {
  if (index < 0 || index >= static_cast<int>(ble_remote::CUSTOM_CHOICE_COUNT)) {
    return;
  }
  SETTINGS.bleRemoteCustomSlots[editingSlot] = static_cast<uint8_t>(ble_remote::CUSTOM_CHOICES[index]);
  dirty = true;
  // Choosing a Custom action implies the Custom profile; otherwise the edit
  // would silently have no effect.
  SETTINGS.bleRemoteProfile = static_cast<uint8_t>(ble_remote::Profile::Custom);
  openView(View::CustomSlots);
}

void BluetoothRemoteSettingsActivity::activatePairedHostRow(int index) {
  if (index < 0 || bondedHosts == 0) {
    return;
  }
  ble_remote::Adapter adapter;
  if (!adapter.begin(ble_remote::AdvertiseMode::BondedOnly)) {
    LOG_ERR(TAG, "could not start adapter to forget host %d", index);
    return;
  }
  if (adapter.forgetBondedHost(static_cast<size_t>(index))) {
    bondedHosts = adapter.bondedHostCount();
  }
  adapter.end();
  rebuildRows();
  requestUpdate();
}

void BluetoothRemoteSettingsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BluetoothRemoteSettingsActivity*>(user);
  if (event.value < 0) {
    return;
  }
  self->selectedIndex = event.value;
  switch (self->view) {
    case View::Root:
      self->activateRootRow(event.value);
      break;
    case View::CustomSlots:
      self->activateCustomSlotRow(event.value);
      break;
    case View::ActionPicker:
      self->activateActionPickerRow(event.value);
      break;
    case View::PairedHosts:
      self->activatePairedHostRow(event.value);
      break;
  }
}

void BluetoothRemoteSettingsActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (view == View::Root) {
      finish();
    } else if (view == View::ActionPicker) {
      openView(View::CustomSlots);
    } else {
      openView(View::Root);
    }
    return;
  }
  if (uiReady) {
    const auto snapshot = touchSnapshotFrom(mappedInput);
    if (snapshot.touchPressed || snapshot.touchReleased) {
      const auto event = app.route(snapshot);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onRowEvent(fui::ActionEvent{ACTION_ROW, static_cast<int16_t>(selectedIndex)}, this);
    return;
  }

  const auto count = static_cast<int>(rowCount);
  if (count <= 0) {
    return;
  }
  const auto move = [this, count](const int index) {
    selectedIndex = index;
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, count);
    requestUpdate();
  };
  buttonNavigator.onNextRelease([this, &move, count] { move(ButtonNavigator::nextIndex(selectedIndex, count)); });
  buttonNavigator.onPreviousRelease(
      [this, &move, count] { move(ButtonNavigator::previousIndex(selectedIndex, count)); });
  buttonNavigator.onNextContinuous(
      [this, &move, count] { move(ButtonNavigator::nextPageIndex(selectedIndex, count, visibleRows)); });
  buttonNavigator.onPreviousContinuous(
      [this, &move, count] { move(ButtonNavigator::previousPageIndex(selectedIndex, count, visibleRows)); });
}

void BluetoothRemoteSettingsActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<BluetoothRemoteSettingsActivity*>(user)->buildListScreen(screen);
}

void BluetoothRemoteSettingsActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (view == View::PairedHosts && bondedHosts > 0) {
    const auto& theme = screen.theme();
    const int16_t lh = screen.target().lineHeight(theme.smallText.font);
    screen.target().text(screen.takeTop(lh, theme.spaceLg), tr(STR_BLE_REMOTE_FORGET_HINT), theme.smallText);
  }

  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(rowCount);
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(rowCount));
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void BluetoothRemoteSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  const char* heading = I18N.get(title());
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, heading, false);
  } else {
    GUI.drawHeader(renderer, header, heading);
  }
  uiReady = false;
  app.render();
  uiReady = true;
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

#endif
