#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "ble/BleRemoteAction.h"

// Transport-facing types shared by the NimBLE adapter, the simulator fake, and
// the native tests. Deliberately free of NimBLE and Arduino headers.
namespace ble_remote {

enum class State : uint8_t {
  Off,           // stack down
  Advertising,   // up, waiting for a host
  Connected,     // linked but not yet authenticated; nothing may be sent
  Ready,         // authenticated and encrypted; reports may be sent
  Failed,        // begin() did not come up
};

// Callbacks run on the NimBLE host task. They may only set atomic state and
// push one of these; the activity drains them from its own loop. Stage 0
// observed onAuthenticationComplete arriving before onConnect on a bonded
// reconnect, so consumers must not assume an order.
enum class Event : uint8_t {
  None,
  Advertising,
  Connected,
  Authenticated,
  Disconnected,
  PasskeyReady,
  Failed,
};

// Single-producer single-consumer ring. The host task pushes, the main loop
// pops. Overflow drops the newest event rather than blocking a callback.
template <typename T, size_t CAPACITY>
class FixedQueue {
 public:
  static constexpr size_t capacity() { return CAPACITY; }

  bool push(T value) {
    const size_t tail = writeIndex.load(std::memory_order_relaxed);
    const size_t next = (tail + 1) % (CAPACITY + 1);
    if (next == readIndex.load(std::memory_order_acquire)) {
      dropped.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    slots[tail] = value;
    writeIndex.store(next, std::memory_order_release);
    return true;
  }

  bool pop(T& out) {
    const size_t head = readIndex.load(std::memory_order_relaxed);
    if (head == writeIndex.load(std::memory_order_acquire)) {
      return false;
    }
    out = slots[head];
    readIndex.store((head + 1) % (CAPACITY + 1), std::memory_order_release);
    return true;
  }

  bool empty() const { return readIndex.load(std::memory_order_acquire) == writeIndex.load(std::memory_order_acquire); }

  size_t size() const {
    const size_t head = readIndex.load(std::memory_order_acquire);
    const size_t tail = writeIndex.load(std::memory_order_acquire);
    return (tail + CAPACITY + 1 - head) % (CAPACITY + 1);
  }

  bool full() const { return size() == CAPACITY; }

  uint32_t droppedCount() const { return dropped.load(std::memory_order_relaxed); }

  void clear() {
    readIndex.store(0, std::memory_order_release);
    writeIndex.store(0, std::memory_order_release);
  }

  void resetDropped() { dropped.store(0, std::memory_order_relaxed); }

 private:
  // One spare slot distinguishes full from empty without a separate count.
  T slots[CAPACITY + 1]{};
  std::atomic<size_t> readIndex{0};
  std::atomic<size_t> writeIndex{0};
  std::atomic<uint32_t> dropped{0};
};

// Spec section 5: eight pending actions, and presses are dropped rather than
// replayed when no host is listening.
inline constexpr size_t ACTION_QUEUE_CAPACITY = 8;
inline constexpr size_t EVENT_QUEUE_CAPACITY = 8;

using ActionQueue = FixedQueue<Action, ACTION_QUEUE_CAPACITY>;
using EventQueue = FixedQueue<Event, EVENT_QUEUE_CAPACITY>;

// How begin() should advertise. Ordinary entry offers itself only to hosts it
// already knows; pairing is an explicit user action.
enum class AdvertiseMode : uint8_t { BondedOnly, PairNewHost };

inline constexpr uint32_t BONDED_ADVERTISE_MS = 30UL * 1000UL;
inline constexpr uint32_t PAIRING_ADVERTISE_MS = 60UL * 1000UL;
inline constexpr size_t MAX_BONDED_HOSTS = 4;

// The transport seam. App code outside src/ble/ never includes NimBLE.
class BleRemoteInterface {
 public:
  virtual ~BleRemoteInterface() = default;

  // Brings the stack up and starts advertising. Returns false and logs on
  // failure; state() then reports Failed.
  virtual bool begin(AdvertiseMode mode) = 0;

  // Releases every held key, drops the link, and tears the stack down. Safe to
  // call when already off.
  virtual void end() = 0;

  // Queues one press-and-release. Returns false when the action is unmapped,
  // no host is Ready, or the queue is saturated.
  virtual bool sendAction(Action action) = 0;

  // Empty keyboard report plus zero consumer report. Runs on every exit path.
  virtual void releaseAll() = 0;

  virtual State state() const = 0;

  // Drains the action queue onto the wire and returns the next pending event,
  // or Event::None. Called from the activity loop, never from a callback.
  virtual Event poll() = 0;

  // Six-digit code to display while pairing, or 0 when none is pending.
  virtual uint32_t passkey() const = 0;

  virtual size_t bondedHostCount() const = 0;
  virtual bool forgetBondedHost(size_t index) = 0;
  virtual bool forgetAllBondedHosts() = 0;
};

}  // namespace ble_remote
