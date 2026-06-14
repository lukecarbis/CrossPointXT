#include "BluetoothKeyboardInput.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#if __has_include(<NimBLEDevice.h>)
#include <NimBLEDevice.h>
#include <host/ble_sm.h>
#define CROSSPOINT_HAS_NIMBLE 1
#else
#define CROSSPOINT_HAS_NIMBLE 0
#endif

void bluetoothKeyboardInputHandleReport(const uint8_t* data, size_t length);
void bluetoothKeyboardInputSetConnected(bool value);
void bluetoothKeyboardInputSetConnecting(bool value);
void bluetoothKeyboardInputSetScanning(bool value);
void bluetoothKeyboardInputSetScanRequested(bool value);
void bluetoothKeyboardInputRecordDevice(bool candidate);
void bluetoothKeyboardInputRecordCandidate(const BluetoothKeyboardInput::Candidate& candidate);
void bluetoothKeyboardInputRequestAutoConnect(const BluetoothKeyboardInput::Candidate& candidate);

namespace {
constexpr uint16_t HID_SERVICE_UUID = 0x1812;
constexpr uint16_t HID_PROTOCOL_MODE_UUID = 0x2A4E;
constexpr uint16_t HID_BOOT_KEYBOARD_INPUT_UUID = 0x2A22;
constexpr uint16_t HID_REPORT_UUID = 0x2A4D;
constexpr uint8_t HID_BOOT_PROTOCOL = 0x00;
constexpr uint16_t APPEARANCE_GENERIC_HID = 0x03C0;
constexpr uint16_t APPEARANCE_KEYBOARD = 0x03C1;
constexpr unsigned long SCAN_INTERVAL_MS = 30000;
constexpr unsigned long SAVED_KEYBOARD_SCAN_INTERVAL_MS = 1000;
constexpr unsigned long SCAN_DURATION_MS = 8000;
constexpr uint32_t CONNECT_TIMEOUT_MS = 8000;
constexpr unsigned long WORD_HOLD_DELAY_MS = 1000;
constexpr unsigned long WORD_REPEAT_MS = 1000;

portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

#if CROSSPOINT_HAS_NIMBLE
NimBLEClient* connectedClient = nullptr;

bool containsKeyboardWord(std::string name) {
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return name.find("keyboard") != std::string::npos || name.find("keychron") != std::string::npos ||
         name.find("keys") != std::string::npos || name.find("kbd") != std::string::npos ||
         name.find("logi") != std::string::npos || name.find("magic keyboard") != std::string::npos;
}

bool isKeyboardCandidate(const NimBLEAdvertisedDevice* advertisedDevice) {
  if (advertisedDevice->isAdvertisingService(NimBLEUUID(HID_SERVICE_UUID))) return true;
  if (advertisedDevice->haveAppearance()) {
    const uint16_t appearance = advertisedDevice->getAppearance();
    if (appearance == APPEARANCE_GENERIC_HID || appearance == APPEARANCE_KEYBOARD) return true;
  }
  return advertisedDevice->haveName() && containsKeyboardWord(advertisedDevice->getName());
}

void notifyCallback(NimBLERemoteCharacteristic* characteristic, uint8_t* data, size_t length, bool /*isNotify*/) {
  LOG_DBG("BTK", "HID report uuid=%s len=%u first=%02X",
          characteristic ? characteristic->getUUID().toString().c_str() : "", static_cast<unsigned>(length),
          length > 0 ? data[0] : 0);
  bluetoothKeyboardInputHandleReport(data, length);
}

class KeyboardClientCallbacks : public NimBLEClientCallbacks {
 public:
  void onDisconnect(NimBLEClient* client, int reason) override {
    LOG_INF("BTK", "BLE HID keyboard disconnected, reason=%d", reason);
    if (client == connectedClient) {
      connectedClient = nullptr;
      bluetoothKeyboardInputSetConnected(false);
      bluetoothKeyboardInputSetConnecting(false);
      bluetoothKeyboardInputSetScanRequested(true);
    }
  }
};

KeyboardClientCallbacks clientCallbacks;

bool connectKeyboardAddress(const NimBLEAddress& address, const char* name) {
  if (connectedClient != nullptr) return true;

  NimBLEDevice::getScan()->stop();
  bluetoothKeyboardInputSetScanning(false);
  bluetoothKeyboardInputSetConnecting(true);

  auto* client = NimBLEDevice::createClient();
  if (!client) {
    bluetoothKeyboardInputSetConnecting(false);
    bluetoothKeyboardInputSetScanRequested(true);
    return false;
  }

  client->setClientCallbacks(&clientCallbacks, false);
  client->setConnectTimeout(CONNECT_TIMEOUT_MS);
  client->setConnectRetries(0);
  client->setConnectionParams(24, 48, 0, 60);

  LOG_INF("BTK", "Connecting to BLE keyboard candidate '%s' (%s, type=%u, timeout=%lu ms)", name,
          address.toString().c_str(), address.getType(), CONNECT_TIMEOUT_MS);
  if (!client->connect(address)) {
    LOG_ERR("BTK", "Connect failed, rc=%d", client->getLastError());
    NimBLEDevice::deleteClient(client);
    bluetoothKeyboardInputSetConnecting(false);
    bluetoothKeyboardInputSetScanRequested(true);
    return false;
  }

  auto* hidService = client->getService(NimBLEUUID(HID_SERVICE_UUID));
  if (!hidService) {
    LOG_ERR("BTK", "Connected candidate has no HID service");
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    bluetoothKeyboardInputSetConnecting(false);
    bluetoothKeyboardInputSetScanRequested(true);
    return false;
  }

  client->secureConnection(false);

  if (auto* protocolMode = hidService->getCharacteristic(NimBLEUUID(HID_PROTOCOL_MODE_UUID))) {
    protocolMode->writeValue(&HID_BOOT_PROTOCOL, 1, true);
  }

  int subscribedReports = 0;
  if (auto* inputReport = hidService->getCharacteristic(NimBLEUUID(HID_BOOT_KEYBOARD_INPUT_UUID))) {
    if ((inputReport->canNotify() || inputReport->canIndicate()) && inputReport->subscribe(true, notifyCallback)) {
      subscribedReports++;
      LOG_INF("BTK", "Subscribed to boot keyboard input report");
    }
  }

  for (auto* characteristic : hidService->getCharacteristics(true)) {
    if (!characteristic || characteristic->getUUID() != NimBLEUUID(HID_REPORT_UUID)) continue;
    LOG_DBG("BTK", "HID report characteristic handle=%u notify=%d indicate=%d", characteristic->getHandle(),
            characteristic->canNotify(), characteristic->canIndicate());
    if ((characteristic->canNotify() || characteristic->canIndicate()) &&
        characteristic->subscribe(true, notifyCallback)) {
      subscribedReports++;
      LOG_INF("BTK", "Subscribed to HID report characteristic handle=%u", characteristic->getHandle());
    }
  }

  if (subscribedReports == 0) {
    LOG_ERR("BTK", "Could not subscribe to any keyboard input report");
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    bluetoothKeyboardInputSetConnecting(false);
    bluetoothKeyboardInputSetScanRequested(true);
    return false;
  }

  connectedClient = client;
  bluetoothKeyboardInputSetConnecting(false);
  bluetoothKeyboardInputSetConnected(true);
  BluetoothKeyboardInput::Candidate connectedKeyboard;
  std::strncpy(connectedKeyboard.name, name, sizeof(connectedKeyboard.name) - 1);
  std::strncpy(connectedKeyboard.address, address.toString().c_str(), sizeof(connectedKeyboard.address) - 1);
  connectedKeyboard.addressType = address.getType();
  BT_KEYBOARD.saveKeyboard(connectedKeyboard);
  BT_KEYBOARD.clearCandidates();
  LOG_INF("BTK", "BLE HID keyboard connected");
  return true;
}

class KeyboardAdvertisedDeviceCallbacks : public NimBLEScanCallbacks {
 public:
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    BluetoothKeyboardInput::Candidate keyboardCandidate;
    const std::string name = advertisedDevice->haveName() ? advertisedDevice->getName() : "Unnamed keyboard";
    const NimBLEAddress& address = advertisedDevice->getAddress();
    std::strncpy(keyboardCandidate.name, name.c_str(), sizeof(keyboardCandidate.name) - 1);
    std::strncpy(keyboardCandidate.address, address.toString().c_str(), sizeof(keyboardCandidate.address) - 1);
    keyboardCandidate.addressType = address.getType();
    keyboardCandidate.rssi = advertisedDevice->getRSSI();

    const bool reconnectMode = BT_KEYBOARD.hasSavedKeyboardProfile();
    const bool advertisedKeyboard = isKeyboardCandidate(advertisedDevice);
    const bool savedKeyboard = BT_KEYBOARD.matchesSavedKeyboard(keyboardCandidate);
    const bool bondedDevice = NimBLEDevice::isBonded(address);
    const bool candidate = advertisedKeyboard || savedKeyboard || bondedDevice;
    bluetoothKeyboardInputRecordDevice(candidate);
    LOG_DBG("BTK",
            "Scan result: name='%s' address=%s type=%u appearance=0x%04X hid=%d saved=%d bonded=%d candidate=%d "
            "connectable=%d rssi=%d",
            advertisedDevice->haveName() ? advertisedDevice->getName().c_str() : "", address.toString().c_str(),
            address.getType(), advertisedDevice->getAppearance(),
            advertisedDevice->isAdvertisingService(NimBLEUUID(HID_SERVICE_UUID)), savedKeyboard, bondedDevice,
            candidate, advertisedDevice->isConnectable(), advertisedDevice->getRSSI());

    if (!candidate || connectedClient != nullptr) return;

    if (!reconnectMode) {
      bluetoothKeyboardInputRecordCandidate(keyboardCandidate);
    }

    if ((savedKeyboard || bondedDevice) && advertisedDevice->isConnectable()) {
      const char* savedKeyboardName = BT_KEYBOARD.getSavedKeyboardName();
      const char* connectName = savedKeyboardName[0] != '\0' ? savedKeyboardName : keyboardCandidate.name;
      LOG_INF("BTK", "Queueing auto-connect for %s keyboard '%s' (%s)", savedKeyboard ? "saved" : "bonded", connectName,
              keyboardCandidate.address);
      std::strncpy(keyboardCandidate.name, connectName, sizeof(keyboardCandidate.name) - 1);
      bluetoothKeyboardInputRequestAutoConnect(keyboardCandidate);
      NimBLEDevice::getScan()->stop();
      bluetoothKeyboardInputSetScanning(false);
    } else if ((savedKeyboard || bondedDevice) && !advertisedDevice->isConnectable()) {
      LOG_DBG("BTK", "Saved/bonded keyboard advertisement is not connectable yet");
    }
  }

  void onScanEnd(const NimBLEScanResults& scanResults, int reason) override {
    bluetoothKeyboardInputSetScanning(false);
    LOG_DBG("BTK", "Scan ended: count=%d reason=%d", scanResults.getCount(), reason);
  }
};

KeyboardAdvertisedDeviceCallbacks scanCallbacks;
#endif
}  // namespace

BluetoothKeyboardInput BT_KEYBOARD;

void bluetoothKeyboardInputHandleReport(const uint8_t* data, const size_t length) {
  BT_KEYBOARD.handleBootKeyboardReport(data, length);
}

void bluetoothKeyboardInputSetConnected(const bool value) {
  BT_KEYBOARD.connected = value;
  BT_KEYBOARD.bumpStatus();
}

void bluetoothKeyboardInputSetConnecting(const bool value) {
  BT_KEYBOARD.connecting = value;
  BT_KEYBOARD.bumpStatus();
}

void bluetoothKeyboardInputSetScanning(const bool value) {
  BT_KEYBOARD.scanning = value;
  BT_KEYBOARD.bumpStatus();
}

void bluetoothKeyboardInputSetScanRequested(const bool value) {
  BT_KEYBOARD.scanRequested = value;
  BT_KEYBOARD.bumpStatus();
}

void bluetoothKeyboardInputSetUnavailable(const bool value) {
  BT_KEYBOARD.unavailable = value;
  BT_KEYBOARD.bumpStatus();
}

void bluetoothKeyboardInputRecordDevice(const bool candidate) {
  BT_KEYBOARD.devicesSeen = BT_KEYBOARD.devicesSeen + 1;
  if (candidate) BT_KEYBOARD.candidatesSeen = BT_KEYBOARD.candidatesSeen + 1;
  BT_KEYBOARD.bumpStatus();
}

void bluetoothKeyboardInputRecordCandidate(const BluetoothKeyboardInput::Candidate& candidate) {
  BT_KEYBOARD.recordCandidate(candidate);
}

void bluetoothKeyboardInputRequestAutoConnect(const BluetoothKeyboardInput::Candidate& candidate) {
  BT_KEYBOARD.requestAutoConnect(candidate);
}

void BluetoothKeyboardInput::begin() {
  clearQueue();
  clearCandidates();
  loadSavedKeyboard();
  memset(previousKeys, 0, sizeof(previousKeys));
  connected = false;
  connecting = false;
  scanning = false;
  scanRequested = true;
  unavailable = false;
  devicesSeen = 0;
  candidatesSeen = 0;
  hasPendingAutoConnect = false;
  pendingAutoConnectDeferred = false;
  deleteKeyHeld = false;
  deleteKeyDidWordDelete = false;
  heldDeleteKeyCode = 0;
  cursorArrowKeyHeld = false;
  heldCursorArrowKeyCode = 0;
  bumpStatus();

#if CROSSPOINT_HAS_NIMBLE
  if (!started) {
    NimBLEDevice::init("CrossPoint");
    NimBLEDevice::setSecurityIOCap(BLE_SM_IO_CAP_NO_IO);
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    auto* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&scanCallbacks, false);
    scan->setActiveScan(true);
    scan->setInterval(45);
    scan->setWindow(30);
    started = true;
  }

  if (connectedClient != nullptr) {
    connected = true;
    connecting = false;
    scanning = false;
    scanRequested = false;
    bumpStatus();
    return;
  }
#else
  unavailable = true;
#endif
}

void BluetoothKeyboardInput::stop() {
#if CROSSPOINT_HAS_NIMBLE
  if (started) {
    NimBLEDevice::getScan()->stop();
    if (connectedClient) {
      connectedClient->disconnect();
      NimBLEDevice::deleteClient(connectedClient);
      connectedClient = nullptr;
    }
  }
#endif
  connected = false;
  connecting = false;
  scanning = false;
  scanRequested = false;
  hasPendingAutoConnect = false;
  pendingAutoConnectDeferred = false;
  deleteKeyHeld = false;
  deleteKeyDidWordDelete = false;
  heldDeleteKeyCode = 0;
  cursorArrowKeyHeld = false;
  heldCursorArrowKeyCode = 0;
  bumpStatus();
  clearQueue();
}

void BluetoothKeyboardInput::update() {
#if CROSSPOINT_HAS_NIMBLE
  if (!started || unavailable) return;

  if (connectedClient != nullptr) {
    if (!connected) {
      connected = true;
      bumpStatus();
    }
    processDeleteKeyHold();
    processCursorArrowKeyHold();
    return;
  }

  Candidate autoConnectCandidate;
  bool shouldAutoConnect = false;
  bool deferAutoConnect = false;
  taskENTER_CRITICAL(&stateMux);
  if (hasPendingAutoConnect) {
    if (pendingAutoConnectDeferred) {
      autoConnectCandidate = pendingAutoConnect;
      hasPendingAutoConnect = false;
      pendingAutoConnectDeferred = false;
      shouldAutoConnect = true;
    } else {
      pendingAutoConnectDeferred = true;
      deferAutoConnect = true;
    }
  }
  taskEXIT_CRITICAL(&stateMux);

  if (deferAutoConnect) return;

  if (shouldAutoConnect) {
    connectKeyboardAddress(NimBLEAddress(std::string(autoConnectCandidate.address), autoConnectCandidate.addressType),
                           autoConnectCandidate.name);
    return;
  }

  if (connecting || scanning) return;

  if (connected) {
    LOG_DBG("BTK", "Keyboard client missing; returning to scan mode");
    connected = false;
    deleteKeyHeld = false;
    scanRequested = true;
    bumpStatus();
  }

  const unsigned long now = millis();
  const unsigned long scanInterval = hasSavedKeyboard ? SAVED_KEYBOARD_SCAN_INTERVAL_MS : SCAN_INTERVAL_MS;
  if (!connected && (scanRequested || now - lastScanStart >= scanInterval)) {
    scanRequested = false;
    lastScanStart = now;
    LOG_DBG("BTK", "Starting BLE keyboard scan for %lu ms", SCAN_DURATION_MS);
    scanning = NimBLEDevice::getScan()->start(SCAN_DURATION_MS, false, true);
    bumpStatus();
  }
#endif
}

bool BluetoothKeyboardInput::isAvailable() const { return !unavailable; }

bool BluetoothKeyboardInput::isConnected() const { return connected; }

int BluetoothKeyboardInput::getCandidateCount() const {
  int count = 0;
  taskENTER_CRITICAL(&stateMux);
  count = candidateCount;
  taskEXIT_CRITICAL(&stateMux);
  return count;
}

bool BluetoothKeyboardInput::getCandidate(const int index, Candidate& candidate) const {
  bool found = false;
  taskENTER_CRITICAL(&stateMux);
  if (index >= 0 && index < candidateCount) {
    candidate = candidates[index];
    found = true;
  }
  taskEXIT_CRITICAL(&stateMux);
  return found;
}

void BluetoothKeyboardInput::selectNextCandidate() {
  taskENTER_CRITICAL(&stateMux);
  if (candidateCount > 0) selectedCandidateIndex = (selectedCandidateIndex + 1) % candidateCount;
  taskEXIT_CRITICAL(&stateMux);
  bumpStatus();
}

void BluetoothKeyboardInput::selectPreviousCandidate() {
  taskENTER_CRITICAL(&stateMux);
  if (candidateCount > 0) selectedCandidateIndex = (selectedCandidateIndex + candidateCount - 1) % candidateCount;
  taskEXIT_CRITICAL(&stateMux);
  bumpStatus();
}

void BluetoothKeyboardInput::requestScan() {
  scanRequested = true;
  bumpStatus();
}

bool BluetoothKeyboardInput::connectSelectedCandidate() {
#if CROSSPOINT_HAS_NIMBLE
  Candidate candidate;
  if (!getCandidate(selectedCandidateIndex, candidate)) {
    requestScan();
    return false;
  }

  return connectKeyboardAddress(NimBLEAddress(std::string(candidate.address), candidate.addressType), candidate.name);
#else
  unavailable = true;
  bumpStatus();
  return false;
#endif
}

bool BluetoothKeyboardInput::popEvent(KeyEvent& event) {
  bool hasEvent = false;
  taskENTER_CRITICAL(&queueMux);
  if (queueTail != queueHead) {
    event = queue[queueTail];
    queueTail = (queueTail + 1) % QUEUE_SIZE;
    hasEvent = true;
  }
  taskEXIT_CRITICAL(&queueMux);
  return hasEvent;
}

const char* BluetoothKeyboardInput::getStatusText() const {
  if (!isAvailable()) return "BLE keyboard support missing";
  if (isConnected()) return "Keyboard connected";
  if (connecting) return "Connecting to keyboard";
  if (hasSavedKeyboard && scanning) {
    snprintf(statusText, sizeof(statusText), "Looking for %s", savedKeyboard.name);
    return statusText;
  }
  if (candidateCount > 0) {
    snprintf(statusText, sizeof(statusText), "Choose keyboard (%u found)", candidateCount);
    return statusText;
  }
  if (scanning) {
    snprintf(statusText, sizeof(statusText), "Scanning for keyboard (%u seen)", devicesSeen);
    return statusText;
  }
  if (devicesSeen > 0) {
    snprintf(statusText, sizeof(statusText), "Pair keyboard (%u seen, %u candidates)", devicesSeen, candidatesSeen);
    return statusText;
  }
  return "Pair a Bluetooth keyboard";
}

void BluetoothKeyboardInput::loadSavedKeyboard() {
  hasSavedKeyboard = false;
  memset(&savedKeyboard, 0, sizeof(savedKeyboard));

  if (!Storage.exists(SAVED_KEYBOARD_PATH)) return;

  const String stored = Storage.readFile(SAVED_KEYBOARD_PATH);
  std::string value(stored.c_str(), stored.length());
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
    value.pop_back();
  }

  const size_t firstSep = value.find('|');
  const size_t secondSep = firstSep == std::string::npos ? std::string::npos : value.find('|', firstSep + 1);
  if (firstSep == std::string::npos || secondSep == std::string::npos) return;

  const std::string name = value.substr(0, firstSep);
  const std::string address = value.substr(firstSep + 1, secondSep - firstSep - 1);
  const std::string type = value.substr(secondSep + 1);
  if (name.empty()) return;

  std::strncpy(savedKeyboard.name, name.c_str(), sizeof(savedKeyboard.name) - 1);
  std::strncpy(savedKeyboard.address, address.c_str(), sizeof(savedKeyboard.address) - 1);
  savedKeyboard.addressType = static_cast<uint8_t>(std::atoi(type.c_str()));
  hasSavedKeyboard = true;
  LOG_INF("BTK", "Loaded saved keyboard '%s' (%s, type=%u)", savedKeyboard.name, savedKeyboard.address,
          savedKeyboard.addressType);
}

void BluetoothKeyboardInput::saveKeyboard(const Candidate& candidate) {
  if (candidate.name[0] == '\0') return;

  Storage.ensureDirectoryExists("/.crosspoint");
  std::string value = candidate.name;
  value += "|";
  value += candidate.address;
  value += "|";
  value += std::to_string(candidate.addressType);

  if (!Storage.writeFile(SAVED_KEYBOARD_PATH, String(value.c_str()))) {
    LOG_ERR("BTK", "Failed to save keyboard pairing");
    return;
  }

  savedKeyboard = candidate;
  hasSavedKeyboard = true;
  LOG_INF("BTK", "Saved keyboard '%s' (%s, type=%u)", savedKeyboard.name, savedKeyboard.address,
          savedKeyboard.addressType);
}

bool BluetoothKeyboardInput::matchesSavedKeyboard(const Candidate& candidate) const {
  if (!hasSavedKeyboard || candidate.name[0] == '\0') return false;
  return std::strcmp(candidate.name, savedKeyboard.name) == 0;
}

const char* BluetoothKeyboardInput::getSavedKeyboardName() const { return hasSavedKeyboard ? savedKeyboard.name : ""; }

void BluetoothKeyboardInput::clearCandidates() {
  taskENTER_CRITICAL(&stateMux);
  candidateCount = 0;
  selectedCandidateIndex = 0;
  memset(candidates, 0, sizeof(candidates));
  taskEXIT_CRITICAL(&stateMux);
}

void BluetoothKeyboardInput::recordCandidate(const Candidate& candidate) {
  taskENTER_CRITICAL(&stateMux);

  int index = -1;
  for (int i = 0; i < candidateCount; i++) {
    if (strncmp(candidates[i].address, candidate.address, sizeof(candidates[i].address)) == 0) {
      index = i;
      break;
    }
  }

  if (index < 0 && candidateCount < MAX_CANDIDATES) {
    index = candidateCount++;
  }

  if (index >= 0) {
    candidates[index] = candidate;
    if (selectedCandidateIndex >= candidateCount) selectedCandidateIndex = candidateCount - 1;
  }

  taskEXIT_CRITICAL(&stateMux);
  bumpStatus();
}

void BluetoothKeyboardInput::requestAutoConnect(const Candidate& candidate) {
  taskENTER_CRITICAL(&stateMux);
  pendingAutoConnect = candidate;
  hasPendingAutoConnect = true;
  pendingAutoConnectDeferred = false;
  taskEXIT_CRITICAL(&stateMux);

  connecting = true;
  scanning = false;
  scanRequested = false;
  bumpStatus();
}

void BluetoothKeyboardInput::clearQueue() {
  taskENTER_CRITICAL(&queueMux);
  queueHead = 0;
  queueTail = 0;
  taskEXIT_CRITICAL(&queueMux);
}

void BluetoothKeyboardInput::pushEvent(const KeyEvent event) {
  taskENTER_CRITICAL(&queueMux);
  const uint8_t nextHead = (queueHead + 1) % QUEUE_SIZE;
  if (nextHead != queueTail) {
    queue[queueHead] = event;
    queueHead = nextHead;
  }
  taskEXIT_CRITICAL(&queueMux);
}

void BluetoothKeyboardInput::pushTextEvent(const char* text) {
  if (!text || text[0] == '\0') return;

  KeyEvent event;
  event.type = KeyType::Character;
  event.character = text[0];
  std::strncpy(event.text, text, sizeof(event.text) - 1);
  pushEvent(event);
}

void BluetoothKeyboardInput::bumpStatus() { statusVersion = statusVersion + 1; }

void BluetoothKeyboardInput::handleBootKeyboardReport(const uint8_t* data, const size_t length) {
  if (!data || length < 8) return;

  if (length >= 9 && data[0] != 0 && data[1] <= 0x22) {
    LOG_DBG("BTK", "Treating HID report as report-id-prefixed keyboard report, id=%u", data[0]);
    data++;
  }

  const uint8_t modifiers = data[0];
  const uint8_t* keys = data + 2;
  bool deleteKeyDown = false;
  uint8_t deleteKeyCode = 0;
  bool cursorArrowKeyDown = false;
  uint8_t cursorArrowKeyCode = 0;
  for (int i = 0; i < 6; i++) {
    const uint8_t keyCode = keys[i];
    if (isDeleteKey(keyCode)) {
      deleteKeyDown = true;
      deleteKeyCode = keyCode;
      continue;
    }
    if (isCursorArrowKey(keyCode)) {
      cursorArrowKeyDown = true;
      cursorArrowKeyCode = keyCode;
    }
    if (keyCode == 0 || keyAlreadyDown(keyCode, previousKeys)) continue;
    emitKey(keyCode, modifiers);
  }

  updateDeleteKeyHold(deleteKeyDown, deleteKeyCode);
  updateCursorArrowKeyHold(cursorArrowKeyDown, cursorArrowKeyCode);
  memcpy(previousKeys, keys, sizeof(previousKeys));
}

void BluetoothKeyboardInput::emitKey(const uint8_t keyCode, const uint8_t modifiers) {
  const bool shifted = (modifiers & 0x22) != 0;        // left/right shift
  const bool optionPressed = (modifiers & 0x44) != 0;  // left/right alt/option
  if (optionPressed) {
    if (const char* text = optionKeyToUtf8(keyCode, shifted); text != nullptr) {
      pushTextEvent(text);
      return;
    }
  }

  if (const char c = keyCodeToAscii(keyCode, shifted); c != '\0') {
    const char text[] = {c, '\0'};
    pushTextEvent(text);
    return;
  }

  switch (keyCode) {
    case 0x28:
      pushEvent(KeyEvent{KeyType::Enter});
      break;
    case 0x29:
      pushEvent(KeyEvent{KeyType::Escape});
      break;
    case 0x2B:
      pushEvent(KeyEvent{KeyType::Tab, '\t'});
      break;
    case 0x4F:
      pushEvent(KeyEvent{KeyType::Right});
      break;
    case 0x50:
      pushEvent(KeyEvent{KeyType::Left});
      break;
    case 0x51:
      pushEvent(KeyEvent{KeyType::Down});
      break;
    case 0x52:
      pushEvent(KeyEvent{KeyType::Up});
      break;
    default:
      break;
  }
}

void BluetoothKeyboardInput::updateDeleteKeyHold(const bool deleteKeyDown, const uint8_t deleteKeyCode) {
  if (deleteKeyDown) {
    if (!deleteKeyHeld) {
      deleteKeyHeld = true;
      deleteKeyDidWordDelete = false;
      heldDeleteKeyCode = deleteKeyCode;
      deleteKeyHoldStartedAt = millis();
      nextDeleteWordAt = deleteKeyHoldStartedAt + WORD_HOLD_DELAY_MS;
    }
    return;
  }

  if (!deleteKeyHeld) return;

  if (!deleteKeyDidWordDelete) {
    pushEvent(KeyEvent{heldDeleteKeyCode == 0x4C ? KeyType::DeleteKey : KeyType::Backspace});
  }
  deleteKeyHeld = false;
  deleteKeyDidWordDelete = false;
  heldDeleteKeyCode = 0;
}

void BluetoothKeyboardInput::processDeleteKeyHold() {
  if (!deleteKeyHeld) return;

  const unsigned long now = millis();
  if (static_cast<long>(now - nextDeleteWordAt) < 0) return;

  pushEvent(KeyEvent{heldDeleteKeyCode == 0x4C ? KeyType::DeleteForwardWord : KeyType::DeleteWord});
  deleteKeyDidWordDelete = true;
  nextDeleteWordAt = now + WORD_REPEAT_MS;
}

void BluetoothKeyboardInput::updateCursorArrowKeyHold(const bool cursorArrowKeyDown, const uint8_t cursorArrowKeyCode) {
  if (cursorArrowKeyDown) {
    if (!cursorArrowKeyHeld || heldCursorArrowKeyCode != cursorArrowKeyCode) {
      cursorArrowKeyHeld = true;
      heldCursorArrowKeyCode = cursorArrowKeyCode;
      cursorArrowKeyHoldStartedAt = millis();
      nextCursorArrowWordAt = cursorArrowKeyHoldStartedAt + WORD_HOLD_DELAY_MS;
    }
    return;
  }

  cursorArrowKeyHeld = false;
  heldCursorArrowKeyCode = 0;
}

void BluetoothKeyboardInput::processCursorArrowKeyHold() {
  if (!cursorArrowKeyHeld) return;

  const unsigned long now = millis();
  if (static_cast<long>(now - nextCursorArrowWordAt) < 0) return;

  switch (heldCursorArrowKeyCode) {
    case 0x4F:
      pushEvent(KeyEvent{KeyType::RightWord});
      break;
    case 0x50:
      pushEvent(KeyEvent{KeyType::LeftWord});
      break;
    case 0x51:
      pushEvent(KeyEvent{KeyType::Down});
      break;
    case 0x52:
      pushEvent(KeyEvent{KeyType::Up});
      break;
    default:
      break;
  }
  nextCursorArrowWordAt = now + WORD_REPEAT_MS;
}

const char* BluetoothKeyboardInput::optionKeyToUtf8(const uint8_t keyCode, const bool shifted) {
  if (keyCode == 0x2D) {
    return shifted ? "—" : "–";
  }
  if (keyCode == 0x33) {
    return "…";
  }

  struct OptionLetter {
    uint8_t keyCode;
    const char* option;
    const char* optionShift;
  };

  static constexpr OptionLetter letters[] = {
      {0x04, "à", "á"},  // a
      {0x06, "ç", "ć"},  // c
      {0x07, "ḏ", "ď"},  // d
      {0x08, "è", "é"},  // e
      {0x0A, "ḡ", "ǵ"},  // g
      {0x0B, "ẖ", "ĥ"},  // h
      {0x0C, "ì", "í"},  // i
      {0x0F, "ḻ", "ł"},  // l
      {0x10, "ṁ", "ḿ"},  // m
      {0x11, "ǹ", "ń"},  // n
      {0x12, "ò", "ó"},  // o
      {0x15, "ṟ", "ŕ"},  // r
      {0x16, "ṣ", "ś"},  // s
      {0x17, "ṯ", "ť"},  // t
      {0x18, "ù", "ú"},  // u
      {0x1A, "ẁ", "ẃ"},  // w
      {0x1C, "ỳ", "ý"},  // y
      {0x1D, "ẓ", "ź"},  // z
  };

  for (const auto& letter : letters) {
    if (letter.keyCode == keyCode) {
      return shifted ? letter.optionShift : letter.option;
    }
  }
  return nullptr;
}

char BluetoothKeyboardInput::keyCodeToAscii(const uint8_t keyCode, const bool shifted) {
  if (keyCode >= 0x04 && keyCode <= 0x1D) {
    const char c = static_cast<char>('a' + keyCode - 0x04);
    return shifted ? static_cast<char>(c - 'a' + 'A') : c;
  }

  static constexpr char normalDigits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};
  static constexpr char shiftedDigits[] = {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'};
  if (keyCode >= 0x1E && keyCode <= 0x27) {
    const int index = keyCode - 0x1E;
    return shifted ? shiftedDigits[index] : normalDigits[index];
  }

  switch (keyCode) {
    case 0x2C:
      return ' ';
    case 0x2D:
      return shifted ? '_' : '-';
    case 0x2E:
      return shifted ? '+' : '=';
    case 0x2F:
      return shifted ? '{' : '[';
    case 0x30:
      return shifted ? '}' : ']';
    case 0x31:
      return shifted ? '|' : '\\';
    case 0x33:
      return shifted ? ':' : ';';
    case 0x34:
      return shifted ? '"' : '\'';
    case 0x35:
      return shifted ? '~' : '`';
    case 0x36:
      return shifted ? '<' : ',';
    case 0x37:
      return shifted ? '>' : '.';
    case 0x38:
      return shifted ? '?' : '/';
    default:
      return '\0';
  }
}

bool BluetoothKeyboardInput::isDeleteKey(const uint8_t keyCode) { return keyCode == 0x2A || keyCode == 0x4C; }

bool BluetoothKeyboardInput::isCursorArrowKey(const uint8_t keyCode) {
  return keyCode == 0x4F || keyCode == 0x50 || keyCode == 0x51 || keyCode == 0x52;
}

bool BluetoothKeyboardInput::keyAlreadyDown(const uint8_t keyCode, const uint8_t* keys) {
  for (int i = 0; i < 6; i++) {
    if (keys[i] == keyCode) return true;
  }
  return false;
}
