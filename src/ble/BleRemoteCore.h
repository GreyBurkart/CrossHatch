#pragma once

#include "ble/BleRemoteTypes.h"

// The transport-independent half of the Bluetooth Remote: the connection state
// machine, the bounded action and event queues, and the rules about when a
// press may be sent. Both the NimBLE adapter and the simulator fake delegate to
// this, so the native tests under test/ble_remote/ cover the real logic rather
// than a copy of it. No NimBLE, Arduino, or FreeRTOS headers.
namespace ble_remote {

// Where drained reports go. A virtual call rather than std::function keeps the
// send path free of type erasure and heap allocation.
class ReportSink {
 public:
  virtual ~ReportSink() = default;
  // Emits one press report followed by one release report. Exactly one pair
  // per queued action; auto-repeat is never synthesized.
  virtual void emitPressRelease(const Report& press) = 0;
};

class BleRemoteCore {
 public:
  State state() const { return currentState.load(std::memory_order_acquire); }
  uint32_t passkey() const { return pendingPasskey.load(std::memory_order_acquire); }
  uint32_t droppedActions() const { return actions.droppedCount(); }
  size_t pendingActions() const { return actions.size(); }

  // Lifecycle, driven by the owning adapter.
  void reset();
  void markAdvertising();
  void markFailed();
  void markOff();

  // Callback-side transitions. Safe to call from the NimBLE host task: they
  // only touch atomics and push into the bounded event queue.
  void onConnected();
  // Returns false when the link is not both encrypted and authenticated, which
  // the caller must treat as a refusal rather than a usable link.
  bool onAuthenticated(bool encrypted, bool authenticated);
  void onDisconnected();
  void setPasskey(uint32_t code);

  // Queues one press. False when the action is unmapped, no authenticated host
  // is listening, or the queue is already holding its eight.
  bool queueAction(Action action);

  // Writes every queued press to the sink and returns how many were sent.
  // Sends nothing unless a host is Ready.
  size_t drainActions(ReportSink& sink);

  // Next pending event, or Event::None.
  Event nextEvent();

 private:
  std::atomic<State> currentState{State::Off};
  std::atomic<uint32_t> pendingPasskey{0};
  EventQueue events;
  ActionQueue actions;
};

}  // namespace ble_remote
