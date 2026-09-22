#include "ble/BleRemote.h"

#if CROSSINK_APP_CAP_BLE_REMOTE && !defined(SIMULATOR)

#include <Arduino.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <esp_random.h>

#include <optional>

namespace ble_remote {
namespace {

constexpr char TAG[] = "BLEREM";
constexpr char DEVICE_NAME[] = "CrossHatch Remote";

constexpr uint8_t REPORT_ID_KEYBOARD = 1;
constexpr uint8_t REPORT_ID_CONSUMER = 2;
constexpr size_t KEYBOARD_REPORT_BYTES = 8;
constexpr size_t CONSUMER_REPORT_BYTES = 2;

// Gap between a press report and its release. One physical press produces
// exactly one of each; auto-repeat is off.
constexpr uint32_t PRESS_RELEASE_GAP_MS = 15;

// One keyboard report (modifiers, reserved, six key slots) and one 16-bit
// consumer-control report. Boot protocol is not required. static const keeps
// the table in flash rather than DRAM.
const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01,                // Usage Page (Generic Desktop)
    0x09, 0x06,                // Usage (Keyboard)
    0xA1, 0x01,                // Collection (Application)
    0x85, REPORT_ID_KEYBOARD,  //   Report ID (1)
    0x05, 0x07,                //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,                //   Usage Minimum (Left Control)
    0x29, 0xE7,                //   Usage Maximum (Right GUI)
    0x15, 0x00,                //   Logical Minimum (0)
    0x25, 0x01,                //   Logical Maximum (1)
    0x75, 0x01,                //   Report Size (1)
    0x95, 0x08,                //   Report Count (8)
    0x81, 0x02,                //   Input (Data, Variable, Absolute) - modifiers
    0x95, 0x01,                //   Report Count (1)
    0x75, 0x08,                //   Report Size (8)
    0x81, 0x03,                //   Input (Constant) - reserved byte
    0x95, 0x06,                //   Report Count (6)
    0x75, 0x08,                //   Report Size (8)
    0x15, 0x00,                //   Logical Minimum (0)
    0x25, 0x65,                //   Logical Maximum (101)
    0x05, 0x07,                //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,                //   Usage Minimum (0)
    0x29, 0x65,                //   Usage Maximum (101)
    0x81, 0x00,                //   Input (Data, Array) - six key slots
    0xC0,                      // End Collection
    0x05, 0x0C,                // Usage Page (Consumer)
    0x09, 0x01,                // Usage (Consumer Control)
    0xA1, 0x01,                // Collection (Application)
    0x85, REPORT_ID_CONSUMER,  //   Report ID (2)
    0x15, 0x00,                //   Logical Minimum (0)
    0x26, 0xFF, 0x03,          //   Logical Maximum (1023)
    0x19, 0x00,                //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,          //   Usage Maximum (1023)
    0x75, 0x10,                //   Report Size (16)
    0x95, 0x01,                //   Report Count (1)
    0x81, 0x00,                //   Input (Data, Array)
    0xC0,                      // End Collection
};

// NimBLEHIDDevice is a non-owning wrapper over services the server owns, so it
// lives in static storage and is re-emplaced per begin() rather than heap
// allocated. NimBLE frees the underlying services in deinit(true).
std::optional<NimBLEHIDDevice> hidDevice;

class ServerCallbacks final : public NimBLEServerCallbacks {
 public:
  void bind(BleRemote* owner) { this->owner = owner; }

  void onConnect(NimBLEServer*, NimBLEConnInfo& connInfo) override {
    LOG_INF(TAG, "connect peer=%s", connInfo.getAddress().toString().c_str());
    if (owner != nullptr) {
      owner->onHostConnected();
    }
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    LOG_INF(TAG, "disconnect reason=%d", reason);
    if (owner != nullptr) {
      owner->onHostDisconnected();
    }
  }

  uint32_t onPassKeyDisplay() override {
    // Stage 0 found that a statically configured passkey bypasses this
    // callback entirely, so the code is generated here instead. That is what
    // lets the reader show a fresh six-digit code per pairing.
    const uint32_t code = owner != nullptr ? owner->generatePasskey() : 0;
    LOG_INF(TAG, "passkey displayed");
    return code;
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    LOG_INF(TAG, "auth encrypted=%d authenticated=%d bonded=%d", static_cast<int>(connInfo.isEncrypted()),
            static_cast<int>(connInfo.isAuthenticated()), static_cast<int>(connInfo.isBonded()));
    if (owner != nullptr) {
      owner->onHostAuthenticated(connInfo.isEncrypted(), connInfo.isAuthenticated());
    }
  }

 private:
  BleRemote* owner = nullptr;
};

ServerCallbacks serverCallbacks;

}  // namespace

BleRemote::~BleRemote() { end(); }

uint32_t BleRemote::generatePasskey() {
  // Uniform over 000000..999999 from the hardware RNG.
  const uint32_t code = esp_random() % 1000000U;
  core.setPasskey(code);
  return code;
}

bool BleRemote::buildServices(AdvertiseMode mode) {
  NimBLEServer* server = NimBLEDevice::createServer();
  if (server == nullptr) {
    LOG_ERR(TAG, "createServer failed");
    return false;
  }
  serverCallbacks.bind(this);
  server->setCallbacks(&serverCallbacks, false);

  hidDevice.emplace(server);
  NimBLEHIDDevice* hid = &hidDevice.value();
  hid->setManufacturer("CrossHatch");
  hid->setPnp(0x02, 0x1209, 0x4350, 0x0100);
  hid->setHidInfo(0x00, 0x01);
  hid->setReportMap(const_cast<uint8_t*>(HID_REPORT_MAP), sizeof(HID_REPORT_MAP));

  keyboardInput = hid->getInputReport(REPORT_ID_KEYBOARD);
  consumerInput = hid->getInputReport(REPORT_ID_CONSUMER);
  if (keyboardInput == nullptr || consumerInput == nullptr) {
    LOG_ERR(TAG, "HID input report characteristics missing");
    return false;
  }

  if (!server->start()) {
    LOG_ERR(TAG, "server start failed");
    return false;
  }

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setAppearance(HID_KEYBOARD);
  advertising->addServiceUUID(hid->getHidService()->getUUID());
  advertising->setName(DEVICE_NAME);
  advertising->enableScanResponse(true);
  advertising->setConnectableMode(BLE_GAP_CONN_MODE_UND);

  if (mode == AdvertiseMode::PairNewHost) {
    advertising->setDiscoverableMode(BLE_GAP_DISC_MODE_GEN);
    advertising->setScanFilter(false, false);
  } else {
    // Ordinary entry offers itself only to hosts already bonded.
    advertising->setDiscoverableMode(BLE_GAP_DISC_MODE_GEN);
    const bool haveBonds = NimBLEDevice::getNumBonds() > 0;
    advertising->setScanFilter(haveBonds, haveBonds);
  }

  const uint32_t durationMs = mode == AdvertiseMode::PairNewHost ? PAIRING_ADVERTISE_MS : BONDED_ADVERTISE_MS;
  if (!advertising->start(durationMs)) {
    LOG_ERR(TAG, "advertising start failed");
    return false;
  }
  return true;
}

bool BleRemote::begin(AdvertiseMode mode) {
  if (core.state() != State::Off) {
    LOG_ERR(TAG, "begin() while already running");
    return false;
  }

  core.reset();
  keyboardInput = nullptr;
  consumerInput = nullptr;
  hidDevice.reset();

  if (!NimBLEDevice::init(DEVICE_NAME)) {
    LOG_ERR(TAG, "NimBLEDevice::init failed");
    core.markFailed();
    return false;
  }

  // Pinned pairing policy: bonding, MITM, and secure connections with a
  // DisplayOnly reader. Just Works cannot satisfy this, so the host is forced
  // to enter the passkey the reader shows. No static passkey is configured, so
  // onPassKeyDisplay() drives the code.
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);

  if (!buildServices(mode)) {
    hidDevice.reset();
    keyboardInput = nullptr;
    consumerInput = nullptr;
    NimBLEDevice::deinit(true);
    core.markFailed();
    return false;
  }

  core.markAdvertising();
  LOG_INF(TAG, "advertising, mode=%s bonds=%d", mode == AdvertiseMode::PairNewHost ? "pair" : "bonded",
          NimBLEDevice::getNumBonds());
  return true;
}

void BleRemote::end() {
  if (core.state() == State::Off) {
    return;
  }
  // The host must never be left holding a key, so release before the link goes.
  releaseAll();
  keyboardInput = nullptr;
  consumerInput = nullptr;
  hidDevice.reset();
  serverCallbacks.bind(nullptr);
  if (!NimBLEDevice::deinit(true)) {
    LOG_ERR(TAG, "NimBLEDevice::deinit(true) reported failure");
  }
  core.markOff();
  LOG_INF(TAG, "stopped");
}

void BleRemote::onHostConnected() { core.onConnected(); }

void BleRemote::onHostAuthenticated(bool encrypted, bool authenticated) {
  if (!core.onAuthenticated(encrypted, authenticated)) {
    LOG_ERR(TAG, "refusing unauthenticated link");
  }
}

void BleRemote::onHostDisconnected() { core.onDisconnected(); }

bool BleRemote::sendAction(Action action) { return core.queueAction(action); }

void BleRemote::writeReport(const Report& report) {
  if (report.kind == ReportKind::Keyboard && keyboardInput != nullptr) {
    uint8_t buffer[KEYBOARD_REPORT_BYTES] = {report.modifiers, 0, report.keycode, 0, 0, 0, 0, 0};
    keyboardInput->setValue(buffer, sizeof(buffer));
    keyboardInput->notify();
  } else if (report.kind == ReportKind::Consumer && consumerInput != nullptr) {
    uint8_t buffer[CONSUMER_REPORT_BYTES] = {static_cast<uint8_t>(report.usage & 0xFF),
                                             static_cast<uint8_t>((report.usage >> 8) & 0xFF)};
    consumerInput->setValue(buffer, sizeof(buffer));
    consumerInput->notify();
  }
}

void BleRemote::emitPressRelease(const Report& press) {
  writeReport(press);
  delay(PRESS_RELEASE_GAP_MS);
  Report release = press;
  release.modifiers = 0;
  release.keycode = 0;
  release.usage = 0;
  writeReport(release);
}

void BleRemote::releaseAll() {
  if (keyboardInput != nullptr) {
    const uint8_t empty[KEYBOARD_REPORT_BYTES] = {0, 0, 0, 0, 0, 0, 0, 0};
    keyboardInput->setValue(empty, sizeof(empty));
    keyboardInput->notify();
  }
  if (consumerInput != nullptr) {
    const uint8_t zero[CONSUMER_REPORT_BYTES] = {0, 0};
    consumerInput->setValue(zero, sizeof(zero));
    consumerInput->notify();
  }
}

Event BleRemote::poll() {
  // Drain queued presses first so a report and its matching release are always
  // written from this task, never from a NimBLE callback.
  core.drainActions(*this);
  return core.nextEvent();
}

size_t BleRemote::bondedHostCount() const {
  const int count = NimBLEDevice::getNumBonds();
  return count > 0 ? static_cast<size_t>(count) : 0;
}

bool BleRemote::forgetBondedHost(size_t index) {
  const int count = NimBLEDevice::getNumBonds();
  if (count <= 0 || index >= static_cast<size_t>(count)) {
    LOG_ERR(TAG, "forgetBondedHost(%u) out of range, bonds=%d", static_cast<unsigned>(index), count);
    return false;
  }
  const NimBLEAddress address = NimBLEDevice::getBondedAddress(static_cast<int>(index));
  if (!NimBLEDevice::deleteBond(address)) {
    LOG_ERR(TAG, "deleteBond failed for %s", address.toString().c_str());
    return false;
  }
  LOG_INF(TAG, "forgot bonded host %s", address.toString().c_str());
  return true;
}

bool BleRemote::forgetAllBondedHosts() {
  if (!NimBLEDevice::deleteAllBonds()) {
    LOG_ERR(TAG, "deleteAllBonds failed");
    return false;
  }
  return true;
}

}  // namespace ble_remote

#endif
