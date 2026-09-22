#pragma once

#include "AppCapabilities.h"

#if CROSSINK_APP_CAP_BLE_REMOTE && defined(SIMULATOR)

#include <atomic>
#include <vector>

#include "ble/BleRemoteCore.h"

namespace ble_remote {

// Deterministic stand-in for the NimBLE adapter. It models the state machine
// and the queue rules exactly, logs every action, and lets a test script drive
// connect, authenticate, and disconnect. It does not simulate radio behavior,
// timing, or range.
class BleRemoteFake final : public BleRemoteInterface, private ReportSink {
 public:
  bool begin(AdvertiseMode mode) override;
  void end() override;
  bool sendAction(Action action) override;
  void releaseAll() override;
  State state() const override { return core.state(); }
  Event poll() override;
  uint32_t passkey() const override { return core.passkey(); }
  size_t bondedHostCount() const override { return bondedHosts; }
  bool forgetBondedHost(size_t index) override;
  bool forgetAllBondedHosts() override;

  // Script hooks. These stand in for the NimBLE host task.
  void scriptConnect();
  void scriptAuthenticate(bool encrypted = true, bool authenticated = true);
  void scriptDisconnect();
  void scriptSetBondedHosts(size_t count) { bondedHosts = count; }

  // Inspection for the native tests and the simulator smoke test.
  const std::vector<Report>& sentReports() const { return sent; }
  uint32_t droppedActions() const { return core.droppedActions(); }
  int releaseAllCount() const { return releaseAlls; }
  AdvertiseMode lastAdvertiseMode() const { return advertiseMode; }
  void clearSent() { sent.clear(); }

 private:
  void emitPressRelease(const Report& press) override;

  BleRemoteCore core;
  AdvertiseMode advertiseMode = AdvertiseMode::BondedOnly;
  size_t bondedHosts = 0;
  int releaseAlls = 0;
  std::vector<Report> sent;
};

}  // namespace ble_remote

#endif
