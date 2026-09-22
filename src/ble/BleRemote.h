#pragma once

#include "AppCapabilities.h"

#if CROSSINK_APP_CAP_BLE_REMOTE && !defined(SIMULATOR)

#include <atomic>

#include "ble/BleRemoteCore.h"

// NimBLE types are forward-declared so no app translation unit outside
// src/ble/BleRemote.cpp ever sees a NimBLE header.
class NimBLECharacteristic;

namespace ble_remote {

// The one adapter over NimBLE-Arduino. Callbacks run on the NimBLE host task
// and only touch the atomics and the event queue below; every report is written
// from poll(), which the activity calls on the main loop.
class BleRemote final : public BleRemoteInterface, private ReportSink {
 public:
  BleRemote() = default;
  ~BleRemote() override;

  BleRemote(const BleRemote&) = delete;
  BleRemote& operator=(const BleRemote&) = delete;

  bool begin(AdvertiseMode mode) override;
  void end() override;
  bool sendAction(Action action) override;
  void releaseAll() override;
  State state() const override { return core.state(); }
  Event poll() override;
  uint32_t passkey() const override { return core.passkey(); }
  size_t bondedHostCount() const override;
  bool forgetBondedHost(size_t index) override;
  bool forgetAllBondedHosts() override;

  // Called only from the NimBLE host task, by the callback object in the .cpp.
  void onHostConnected();
  void onHostAuthenticated(bool encrypted, bool authenticated);
  void onHostDisconnected();
  uint32_t generatePasskey();

 private:
  bool buildServices(AdvertiseMode mode);
  void writeReport(const Report& report);
  void emitPressRelease(const Report& press) override;

  BleRemoteCore core;

  // Owned by NimBLE; cleared on end() and never dereferenced while Off.
  NimBLECharacteristic* keyboardInput = nullptr;
  NimBLECharacteristic* consumerInput = nullptr;
};

}  // namespace ble_remote

#endif
