#include "activities/RemoteActivity.h"

#if CROSSINK_APP_CAP_BLE_REMOTE

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "ble/BleRemoteStrings.h"
#include "components/UiAppHelpers.h"

#if !defined(SIMULATOR)
#include <WiFi.h>
#endif

namespace fui = freeink::ui;
using ble_remote::Action;
using ble_remote::Slot;

namespace {

constexpr char TAG[] = "REMOTE";
constexpr fui::ActionId ACTION_TILE = 1;

// Tile order, three per row. The top row is the core clicker; the bottom row
// is secondary and is dropped entirely when the profile maps none of it.
constexpr Slot TILE_ORDER[ble_remote::SLOT_COUNT] = {Slot::Previous, Slot::Primary, Slot::Next,
                                                     Slot::Escape,   Slot::Aux1,    Slot::Aux2};
constexpr uint16_t CORE_TILE_COUNT = 3;

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

// Another radio owner would fight NimBLE for the antenna and the internal
// heap. The repo has no shared "network busy" flag to reuse, so this checks
// the one observable that matters: whether Wi-Fi is still up.
bool radioBusy() {
#if defined(SIMULATOR)
  return false;
#else
  return WiFi.getMode() != WIFI_MODE_NULL;
#endif
}

}  // namespace

RemoteActivity::RemoteActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool pairNewHost)
    : Activity("Remote", renderer, mappedInput),
      pairNewHost(pairNewHost),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void RemoteActivity::onEnter() {
  Activity::onEnter();
  uiReady = false;
  startFailed = false;
  authRefused = false;
  backLongPressFired = false;
  passkeyText[0] = '\0';

  wifiBusy = radioBusy();
  if (wifiBusy) {
    LOG_ERR(TAG, "refusing to start: another radio owner is active");
  } else {
    const auto mode = pairNewHost ? ble_remote::AdvertiseMode::PairNewHost : ble_remote::AdvertiseMode::BondedOnly;
    if (!remote.begin(mode)) {
      startFailed = true;
      LOG_ERR(TAG, "adapter failed to start");
    }
  }

  refreshTiles();
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_TILE, &RemoteActivity::onTileEvent, this);
  app.setScreen(&RemoteActivity::remoteScreen, this);
  requestUpdate();
}

void RemoteActivity::onExit() {
  // end() releases every held key before dropping the link, so the host is
  // never left with a stuck modifier or key.
  remote.releaseAll();
  remote.end();
  Activity::onExit();
}

bool RemoteActivity::preventAutoSleep() {
  return SETTINGS.bleRemoteKeepAwake != 0 && remote.state() == ble_remote::State::Ready;
}

void RemoteActivity::sendSlot(Slot slot) {
  const auto profile = static_cast<ble_remote::Profile>(SETTINGS.bleRemoteProfile);
  const Action action =
      ble_remote::resolveSlot(profile, slot, SETTINGS.bleRemoteCustomSlots, std::size(SETTINGS.bleRemoteCustomSlots));
  if (action == Action::None) {
    return;
  }
  // A refusal here is an unmapped action, a host that is not ready, or a
  // saturated queue. All three are drops by design, not errors to surface.
  (void)remote.sendAction(action);
}

void RemoteActivity::drainAdapterEvents() {
  bool needsRedraw = false;
  for (auto event = remote.poll(); event != ble_remote::Event::None; event = remote.poll()) {
    switch (event) {
      case ble_remote::Event::PasskeyReady:
        snprintf(passkeyText, sizeof(passkeyText), "%06u", static_cast<unsigned>(remote.passkey()));
        needsRedraw = true;
        break;
      case ble_remote::Event::Authenticated:
        passkeyText[0] = '\0';
        authRefused = false;
        needsRedraw = true;
        break;
      case ble_remote::Event::Failed:
        authRefused = true;
        needsRedraw = true;
        break;
      case ble_remote::Event::Advertising:
      case ble_remote::Event::Connected:
      case ble_remote::Event::Disconnected:
        needsRedraw = true;
        break;
      case ble_remote::Event::None:
        break;
    }
  }
  if (needsRedraw) {
    // Redraw only on a state change, never per press.
    refreshTiles();
    requestUpdate();
  }
}

const char* RemoteActivity::statusText() const {
  if (wifiBusy) return tr(STR_BLE_REMOTE_BUSY);
  if (startFailed) return tr(STR_BLE_REMOTE_START_FAILED);
  if (authRefused) return tr(STR_BLE_REMOTE_AUTH_REFUSED);
  switch (remote.state()) {
    case ble_remote::State::Ready:
      return tr(STR_BLE_REMOTE_CONNECTED);
    case ble_remote::State::Connected:
      return tr(STR_BLE_REMOTE_CONNECTING);
    case ble_remote::State::Advertising:
      return pairNewHost ? tr(STR_BLE_REMOTE_PAIRING) : tr(STR_BLE_REMOTE_ADVERTISING);
    case ble_remote::State::Failed:
      return tr(STR_BLE_REMOTE_START_FAILED);
    case ble_remote::State::Off:
      break;
  }
  return tr(STR_BLE_REMOTE_NOT_CONNECTED);
}

const char* RemoteActivity::profileName() const {
  switch (static_cast<ble_remote::Profile>(SETTINGS.bleRemoteProfile)) {
    case ble_remote::Profile::Media:
      return tr(STR_BLE_REMOTE_PROFILE_MEDIA);
    case ble_remote::Profile::Navigation:
      return tr(STR_BLE_REMOTE_PROFILE_NAVIGATION);
    case ble_remote::Profile::Custom:
      return tr(STR_BLE_REMOTE_PROFILE_CUSTOM);
    case ble_remote::Profile::Presentation:
      break;
  }
  return tr(STR_BLE_REMOTE_PROFILE_PRESENTATION);
}

void RemoteActivity::refreshTiles() {
  const auto profile = static_cast<ble_remote::Profile>(SETTINGS.bleRemoteProfile);
  bool secondRowUsed = false;
  for (size_t i = 0; i < ble_remote::SLOT_COUNT; ++i) {
    const Slot slot = TILE_ORDER[i];
    const Action action = ble_remote::resolveSlot(profile, slot, SETTINGS.bleRemoteCustomSlots,
                                                  std::size(SETTINGS.bleRemoteCustomSlots));
    // The action is the label: with a Custom profile a slot name like "Aux 1"
    // says nothing, while "Volume Up" says exactly what the tile sends.
    tiles[i].label = I18N.get(ble_remote::actionLabel(action));
    tiles[i].value = static_cast<int16_t>(i);
    tiles[i].enabled = action != Action::None;
    if (i >= CORE_TILE_COUNT && action != Action::None) {
      secondRowUsed = true;
    }
  }
  // 2x3 when the secondary row carries anything, 1x3 when it is all unset.
  tileCount = secondRowUsed ? static_cast<uint16_t>(ble_remote::SLOT_COUNT) : CORE_TILE_COUNT;
}

void RemoteActivity::onTileEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<RemoteActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int>(ble_remote::SLOT_COUNT)) {
    return;
  }
  self->sendSlot(TILE_ORDER[event.value]);
  // Deliberately no requestUpdate(): the host reacting is the feedback, and an
  // e-ink redraw per press would be both slow and ugly.
}

void RemoteActivity::loop() {
  drainAdapterEvents();

  // Back is the only button that still means something local. A long press
  // leaves; a short press sends Escape to the host.
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && !backLongPressFired &&
      mappedInput.getHeldTime() >= BACK_LONG_PRESS_MS) {
    backLongPressFired = true;
    finishAfterBackPress();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!backLongPressFired) {
      sendSlot(Slot::Escape);
    }
    backLongPressFired = false;
    return;
  }
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finish();
    return;
  }

  if (uiReady) {
    const auto snapshot = touchSnapshotFrom(mappedInput);
    if (snapshot.touchPressed || snapshot.touchReleased) {
      const auto event = app.route(snapshot);
      if (event) return;
    }
  }

  // Spec section 5 physical map. In Remote mode none of these reach the
  // reader, because this activity consumes them here.
  if (mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
      mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    sendSlot(Slot::Next);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    sendSlot(Slot::Previous);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    sendSlot(Slot::Primary);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    sendSlot(Slot::Aux1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    sendSlot(Slot::Aux2);
    return;
  }
  // Power keeps its normal system behavior and is intentionally not handled.
}

void RemoteActivity::remoteScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<RemoteActivity*>(user)->buildRemoteScreen(screen);
}

void RemoteActivity::buildRemoteScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Status band. Redrawn only when the connection state or the profile
  // changes, never on a press.
  const auto& theme = screen.theme();
  const auto statusStyle = theme.bodyText;
  const int16_t statusHeight = screen.target().lineHeight(statusStyle.font);
  screen.target().text(screen.takeTop(statusHeight, theme.spaceLg), statusText(), statusStyle);
  if (passkeyText[0] != '\0') {
    screen.target().text(screen.takeTop(statusHeight), tr(STR_BLE_REMOTE_PASSKEY_HINT), theme.smallText);
    screen.target().text(screen.takeTop(statusHeight), passkeyText, statusStyle);
  }
  screen.target().text(screen.takeTop(statusHeight, theme.spaceLg), profileName(), theme.smallText);

  // Fill the rest of the body with the tiles, so each one is as large a touch
  // target as the screen allows rather than a fixed small height.
  const fui::Rect body = screen.body();
  fui::TileGridProps props;
  props.items = tiles;
  props.count = tileCount;
  props.action = ACTION_TILE;
  props.columns = TILE_COLUMNS;
  props.inputMask = fui::InputTouch;
  props.text = theme.bodyText;
  const int16_t rows = static_cast<int16_t>((tileCount + TILE_COLUMNS - 1) / TILE_COLUMNS);
  if (rows > 0 && body.height > 0) {
    const int16_t available = static_cast<int16_t>(body.height - (rows - 1) * props.gap);
    const int16_t perRow = static_cast<int16_t>(available / rows);
    props.tileHeight = perRow > theme.minTouchSize ? perRow : theme.minTouchSize;
  }
  screen.tileGrid(props);
}

void RemoteActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_BLE_REMOTE), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_BLE_REMOTE));
  }
  uiReady = false;
  app.render();
  uiReady = true;
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)),
                                            I18N.get(slotLabel(Slot::Primary)), I18N.get(slotLabel(Slot::Previous)),
                                            I18N.get(slotLabel(Slot::Next)));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

#endif
