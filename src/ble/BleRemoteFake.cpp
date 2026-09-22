#include "ble/BleRemoteFake.h"

#if CROSSINK_APP_CAP_BLE_REMOTE && defined(SIMULATOR)

#include <Logging.h>

namespace ble_remote {
namespace {
constexpr char TAG[] = "BLEREM";
// Fixed so simulator runs and their expected logs stay reproducible.
constexpr uint32_t FAKE_PASSKEY = 123456;
}  // namespace

bool BleRemoteFake::begin(AdvertiseMode mode) {
  if (core.state() != State::Off) {
    LOG_ERR(TAG, "begin() while already running");
    return false;
  }
  core.reset();
  sent.clear();
  advertiseMode = mode;
  core.markAdvertising();
  LOG_INF(TAG, "fake advertising, mode=%s bonds=%u", mode == AdvertiseMode::PairNewHost ? "pair" : "bonded",
          static_cast<unsigned>(bondedHosts));
  return true;
}

void BleRemoteFake::end() {
  if (core.state() == State::Off) {
    return;
  }
  releaseAll();
  core.markOff();
  LOG_INF(TAG, "fake stopped");
}

bool BleRemoteFake::sendAction(Action action) {
  const bool queued = core.queueAction(action);
  if (!queued) {
    LOG_INF(TAG, "fake dropped action %u", static_cast<unsigned>(action));
  }
  return queued;
}

void BleRemoteFake::releaseAll() {
  ++releaseAlls;
  LOG_INF(TAG, "fake releaseAll");
}

void BleRemoteFake::emitPressRelease(const Report& press) {
  sent.push_back(press);
  LOG_INF(TAG, "fake sent kind=%u key=0x%02X usage=0x%04X", static_cast<unsigned>(press.kind), press.keycode,
          press.usage);
}

Event BleRemoteFake::poll() {
  core.drainActions(*this);
  return core.nextEvent();
}

bool BleRemoteFake::forgetBondedHost(size_t index) {
  if (index >= bondedHosts) {
    LOG_ERR(TAG, "forgetBondedHost(%u) out of range, bonds=%u", static_cast<unsigned>(index),
            static_cast<unsigned>(bondedHosts));
    return false;
  }
  --bondedHosts;
  return true;
}

bool BleRemoteFake::forgetAllBondedHosts() {
  bondedHosts = 0;
  return true;
}

void BleRemoteFake::scriptConnect() {
  if (advertiseMode == AdvertiseMode::PairNewHost) {
    core.setPasskey(FAKE_PASSKEY);
  }
  core.onConnected();
}

void BleRemoteFake::scriptAuthenticate(bool encrypted, bool authenticated) {
  if (!core.onAuthenticated(encrypted, authenticated)) {
    return;
  }
  if (bondedHosts < MAX_BONDED_HOSTS) {
    ++bondedHosts;
  }
}

void BleRemoteFake::scriptDisconnect() { core.onDisconnected(); }

}  // namespace ble_remote

#endif
