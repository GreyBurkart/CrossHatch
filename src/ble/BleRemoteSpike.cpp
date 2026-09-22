#include "ble/BleRemoteSpike.h"

#if defined(CROSSINK_BLE_REMOTE_SPIKE) && CROSSINK_BLE_REMOTE_SPIKE

#include <Arduino.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <esp_heap_caps.h>

#include <atomic>
#include <cstdint>
#include <optional>

namespace BleRemoteSpike {
namespace {

constexpr char TAG[] = "BLESPIKE";

constexpr uint8_t REPORT_ID_KEYBOARD = 1;
constexpr uint8_t REPORT_ID_CONSUMER = 2;

constexpr uint8_t HID_KEY_RIGHT_ARROW = 0x4F;
constexpr uint8_t HID_KEY_LEFT_ARROW = 0x50;
constexpr uint16_t HID_CONSUMER_PLAY_PAUSE = 0x00CD;

constexpr uint32_t SPIKE_PASSKEY = 424242;
constexpr int DEINIT_CYCLES = 10;
constexpr unsigned long PAIR_WINDOW_MS = 120UL * 1000UL;
constexpr unsigned long FOCUS_GRACE_MS = 5UL * 1000UL;
constexpr unsigned long SEND_INTERVAL_MS = 3UL * 1000UL;
constexpr int RIGHT_ARROW_SENDS = 10;
constexpr int LEFT_ARROW_SENDS = 3;

// One keyboard report (8 bytes: modifiers, reserved, six key slots) and one
// 16-bit consumer-control report. Boot protocol is intentionally absent.
// static const keeps the table in flash rather than DRAM.
const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01,                    // Usage Page (Generic Desktop)
    0x09, 0x06,                    // Usage (Keyboard)
    0xA1, 0x01,                    // Collection (Application)
    0x85, REPORT_ID_KEYBOARD,      //   Report ID (1)
    0x05, 0x07,                    //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,                    //   Usage Minimum (Left Control)
    0x29, 0xE7,                    //   Usage Maximum (Right GUI)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x01,                    //   Logical Maximum (1)
    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x08,                    //   Report Count (8)
    0x81, 0x02,                    //   Input (Data, Variable, Absolute) - modifiers
    0x95, 0x01,                    //   Report Count (1)
    0x75, 0x08,                    //   Report Size (8)
    0x81, 0x03,                    //   Input (Constant) - reserved byte
    0x95, 0x06,                    //   Report Count (6)
    0x75, 0x08,                    //   Report Size (8)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x65,                    //   Logical Maximum (101)
    0x05, 0x07,                    //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,                    //   Usage Minimum (0)
    0x29, 0x65,                    //   Usage Maximum (101)
    0x81, 0x00,                    //   Input (Data, Array) - six key slots
    0xC0,                          // End Collection
    0x05, 0x0C,                    // Usage Page (Consumer)
    0x09, 0x01,                    // Usage (Consumer Control)
    0xA1, 0x01,                    // Collection (Application)
    0x85, REPORT_ID_CONSUMER,      //   Report ID (2)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x26, 0xFF, 0x03,              //   Logical Maximum (1023)
    0x19, 0x00,                    //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,              //   Usage Maximum (1023)
    0x75, 0x10,                    //   Report Size (16)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x00,                    //   Input (Data, Array)
    0xC0,                          // End Collection
};

std::atomic<bool> connected{false};
std::atomic<bool> authenticated{false};

void logHeap(const char* phase) {
  const size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t freeDma = heap_caps_get_free_size(MALLOC_CAP_DMA);
  const size_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  LOG_INF(TAG, "HEAP %-18s internalFree=%u internalLargest=%u dmaFree=%u psramFree=%u", phase,
          static_cast<unsigned>(freeInternal), static_cast<unsigned>(largestInternal),
          static_cast<unsigned>(freeDma), static_cast<unsigned>(freePsram));
}

class SpikeServerCallbacks final : public NimBLEServerCallbacks {
 public:
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    (void)server;
    connected.store(true);
    LOG_INF(TAG, "EVENT connect peer=%s", connInfo.getAddress().toString().c_str());
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    (void)server;
    (void)connInfo;
    connected.store(false);
    authenticated.store(false);
    LOG_INF(TAG, "EVENT disconnect reason=%d", reason);
  }

  uint32_t onPassKeyDisplay() override {
    LOG_INF(TAG, "EVENT passkey-display passkey=%06u", static_cast<unsigned>(SPIKE_PASSKEY));
    return SPIKE_PASSKEY;
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    authenticated.store(connInfo.isEncrypted());
    LOG_INF(TAG, "EVENT auth-complete encrypted=%d authenticated=%d bonded=%d",
            static_cast<int>(connInfo.isEncrypted()), static_cast<int>(connInfo.isAuthenticated()),
            static_cast<int>(connInfo.isBonded()));
  }
};

SpikeServerCallbacks serverCallbacks;

// NimBLEHIDDevice is a non-owning wrapper over services the server owns, so it
// lives in static storage rather than on the heap and is simply re-emplaced on
// each cycle. The characteristics below are owned by NimBLE and released by
// deinit(true); the spike only caches the pointers it notifies through.
std::optional<NimBLEHIDDevice> hidDevice;
NimBLECharacteristic* keyboardInput = nullptr;
NimBLECharacteristic* consumerInput = nullptr;

bool bringUpStack(bool discoverable) {
  connected.store(false);
  authenticated.store(false);
  keyboardInput = nullptr;
  consumerInput = nullptr;
  hidDevice.reset();

  if (!NimBLEDevice::init("CrossHatch Remote")) {
    LOG_ERR(TAG, "NimBLEDevice::init failed");
    return false;
  }

  // Pinned pairing policy: bonding + MITM + secure connections with a
  // DisplayOnly reader, which forces passkey entry on the host and refuses
  // Just Works.
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(SPIKE_PASSKEY);

  NimBLEServer* server = NimBLEDevice::createServer();
  if (server == nullptr) {
    LOG_ERR(TAG, "createServer failed");
    NimBLEDevice::deinit(true);
    return false;
  }
  server->setCallbacks(&serverCallbacks, false);

  hidDevice.emplace(server);
  NimBLEHIDDevice* hid = &hidDevice.value();
  hid->setManufacturer("CrossHatch");
  hid->setPnp(0x02, 0x1209, 0x4350, 0x0100);
  hid->setHidInfo(0x00, 0x01);
  hid->setReportMap(const_cast<uint8_t*>(HID_REPORT_MAP), sizeof(HID_REPORT_MAP));
  hid->setBatteryLevel(100);

  keyboardInput = hid->getInputReport(REPORT_ID_KEYBOARD);
  consumerInput = hid->getInputReport(REPORT_ID_CONSUMER);
  if (keyboardInput == nullptr || consumerInput == nullptr) {
    LOG_ERR(TAG, "HID input report characteristics missing");
    hidDevice.reset();
    NimBLEDevice::deinit(true);
    return false;
  }

  if (!server->start()) {
    LOG_ERR(TAG, "server start failed");
    hidDevice.reset();
    NimBLEDevice::deinit(true);
    return false;
  }

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setAppearance(HID_KEYBOARD);
  advertising->addServiceUUID(hid->getHidService()->getUUID());
  advertising->setName("CrossHatch Remote");
  advertising->enableScanResponse(true);
  advertising->setConnectableMode(BLE_GAP_CONN_MODE_UND);
  advertising->setDiscoverableMode(discoverable ? BLE_GAP_DISC_MODE_GEN : BLE_GAP_DISC_MODE_NON);
  if (!advertising->start()) {
    LOG_ERR(TAG, "advertising start failed");
    hidDevice.reset();
    NimBLEDevice::deinit(true);
    return false;
  }
  return true;
}

void tearDownStack() {
  keyboardInput = nullptr;
  consumerInput = nullptr;
  hidDevice.reset();
  if (!NimBLEDevice::deinit(true)) {
    LOG_ERR(TAG, "NimBLEDevice::deinit(true) reported failure");
  }
}

void sendKeyboard(uint8_t keycode) {
  if (keyboardInput == nullptr) {
    return;
  }
  uint8_t report[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  report[2] = keycode;
  keyboardInput->setValue(report, sizeof(report));
  keyboardInput->notify();
  delay(15);
  report[2] = 0;
  keyboardInput->setValue(report, sizeof(report));
  keyboardInput->notify();
  LOG_INF(TAG, "SEND keyboard keycode=0x%02X (press+release)", keycode);
}

void sendConsumer(uint16_t usage) {
  if (consumerInput == nullptr) {
    return;
  }
  uint8_t report[2] = {static_cast<uint8_t>(usage & 0xFF), static_cast<uint8_t>((usage >> 8) & 0xFF)};
  consumerInput->setValue(report, sizeof(report));
  consumerInput->notify();
  delay(15);
  report[0] = 0;
  report[1] = 0;
  consumerInput->setValue(report, sizeof(report));
  consumerInput->notify();
  LOG_INF(TAG, "SEND consumer usage=0x%04X (press+release)", usage);
}

void releaseAll() {
  if (keyboardInput != nullptr) {
    const uint8_t empty[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    keyboardInput->setValue(empty, sizeof(empty));
    keyboardInput->notify();
  }
  if (consumerInput != nullptr) {
    const uint8_t zero[2] = {0, 0};
    consumerInput->setValue(zero, sizeof(zero));
    consumerInput->notify();
  }
  LOG_INF(TAG, "releaseAll sent");
}

void runInteractiveWindow() {
  LOG_INF(TAG, "PAIR window open for %lu s; device name 'CrossHatch Remote', passkey %06u",
          PAIR_WINDOW_MS / 1000UL, static_cast<unsigned>(SPIKE_PASSKEY));
  const unsigned long windowStart = millis();
  while (!connected.load() && (millis() - windowStart) < PAIR_WINDOW_MS) {
    delay(200);
  }
  if (!connected.load()) {
    LOG_INF(TAG, "PAIR window expired with no connection");
    return;
  }
  LOG_INF(TAG, "PAIR connected after %lu ms; reconnect/connect time recorded", millis() - windowStart);
  logHeap("connected");

  LOG_INF(TAG, "Focus the target app now; first key in %lu s", FOCUS_GRACE_MS / 1000UL);
  delay(FOCUS_GRACE_MS);

  for (int i = 0; i < RIGHT_ARROW_SENDS && connected.load(); ++i) {
    sendKeyboard(HID_KEY_RIGHT_ARROW);
    delay(SEND_INTERVAL_MS);
  }
  for (int i = 0; i < LEFT_ARROW_SENDS && connected.load(); ++i) {
    sendKeyboard(HID_KEY_LEFT_ARROW);
    delay(SEND_INTERVAL_MS);
  }
  if (connected.load()) {
    sendConsumer(HID_CONSUMER_PLAY_PAUSE);
    delay(SEND_INTERVAL_MS);
  }

  releaseAll();
  logHeap("after-send-burst");
  LOG_INF(TAG, "bondedHosts=%d", NimBLEDevice::getNumBonds());
}

}  // namespace

void run() {
  LOG_INF(TAG, "==== Stage 0 BLE spike start ====");
  logHeap("pre-init");

  if (!bringUpStack(true)) {
    LOG_ERR(TAG, "first init failed; aborting spike");
    return;
  }
  logHeap("post-init-1");

  tearDownStack();
  delay(200);
  logHeap("post-deinit-1");

  bool reinitOk = true;
  for (int cycle = 2; cycle <= DEINIT_CYCLES + 1; ++cycle) {
    if (!bringUpStack(false)) {
      LOG_ERR(TAG, "re-init failed on cycle %d; controller memory is not reusable", cycle);
      reinitOk = false;
      break;
    }
    const size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    tearDownStack();
    delay(100);
    const size_t afterFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    LOG_INF(TAG, "CYCLE %2d internalFreeWhileUp=%u internalFreeAfterDeinit=%u", cycle,
            static_cast<unsigned>(freeInternal), static_cast<unsigned>(afterFree));
  }

  logHeap("post-10-cycles");
  LOG_INF(TAG, "re-init after deinit(true): %s", reinitOk ? "WORKS" : "FAILED (one-way release)");

  if (!reinitOk) {
    LOG_INF(TAG, "==== Stage 0 BLE spike end (no pairing window) ====");
    return;
  }

  if (!bringUpStack(true)) {
    LOG_ERR(TAG, "final init for pairing window failed");
    LOG_INF(TAG, "==== Stage 0 BLE spike end ====");
    return;
  }
  logHeap("post-init-final");
  runInteractiveWindow();
  tearDownStack();
  delay(200);
  logHeap("post-final-deinit");
  LOG_INF(TAG, "==== Stage 0 BLE spike end ====");
}

}  // namespace BleRemoteSpike

#endif
