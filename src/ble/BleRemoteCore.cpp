#include "ble/BleRemoteCore.h"

namespace ble_remote {

void BleRemoteCore::reset() {
  events.clear();
  actions.clear();
  actions.resetDropped();
  pendingPasskey.store(0, std::memory_order_release);
  currentState.store(State::Off, std::memory_order_release);
}

void BleRemoteCore::markAdvertising() {
  currentState.store(State::Advertising, std::memory_order_release);
  events.push(Event::Advertising);
}

void BleRemoteCore::markFailed() {
  currentState.store(State::Failed, std::memory_order_release);
  events.push(Event::Failed);
}

void BleRemoteCore::markOff() {
  actions.clear();
  pendingPasskey.store(0, std::memory_order_release);
  currentState.store(State::Off, std::memory_order_release);
}

void BleRemoteCore::onConnected() {
  // Connected is deliberately not Ready. Reports sent before authentication
  // are discarded by the host, so nothing may go out yet.
  State expected = State::Advertising;
  currentState.compare_exchange_strong(expected, State::Connected, std::memory_order_acq_rel);
  events.push(Event::Connected);
}

bool BleRemoteCore::onAuthenticated(bool encrypted, bool authenticated) {
  if (!encrypted || !authenticated) {
    events.push(Event::Failed);
    return false;
  }
  pendingPasskey.store(0, std::memory_order_release);
  // Stage 0 observed this arriving before onConnect on a bonded reconnect, so
  // promote unconditionally rather than from an expected state.
  currentState.store(State::Ready, std::memory_order_release);
  events.push(Event::Authenticated);
  return true;
}

void BleRemoteCore::onDisconnected() {
  // A disconnect while Off means a late callback after teardown; it must not
  // resurrect the state machine.
  if (currentState.load(std::memory_order_acquire) == State::Off) {
    return;
  }
  currentState.store(State::Advertising, std::memory_order_release);
  pendingPasskey.store(0, std::memory_order_release);
  // Anything still queued belongs to a host that is gone.
  actions.clear();
  events.push(Event::Disconnected);
}

void BleRemoteCore::setPasskey(uint32_t code) {
  pendingPasskey.store(code, std::memory_order_release);
  events.push(Event::PasskeyReady);
}

bool BleRemoteCore::queueAction(Action action) {
  if (reportFor(action).isEmpty()) {
    return false;
  }
  if (currentState.load(std::memory_order_acquire) != State::Ready) {
    // Spec section 5: presses are dropped, never queued for later replay.
    return false;
  }
  return actions.push(action);
}

size_t BleRemoteCore::drainActions(ReportSink& sink) {
  if (currentState.load(std::memory_order_acquire) != State::Ready) {
    return 0;
  }
  size_t sent = 0;
  Action action = Action::None;
  while (actions.pop(action)) {
    sink.emitPressRelease(reportFor(action));
    ++sent;
  }
  return sent;
}

Event BleRemoteCore::nextEvent() {
  Event event = Event::None;
  events.pop(event);
  return event;
}

}  // namespace ble_remote
