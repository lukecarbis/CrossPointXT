#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class BluetoothKeyboardInput {
 public:
  enum class KeyType : uint8_t {
    Character,
    Enter,
    Backspace,
    Tab,
    Escape,
    Left,
    Right,
    Up,
    Down,
    DeleteKey,
    DeleteWord,
    DeleteForwardWord,
    LeftWord,
    RightWord,
    DocumentStart,
    DocumentEnd
  };

  struct KeyEvent {
    KeyType type = KeyType::Character;
    char character = '\0';
    char text[8] = {};
  };

  struct Candidate {
    char name[40] = {};
    char address[18] = {};
    uint8_t addressType = 0;
    int rssi = 0;
  };

  void begin();
  void stop();
  void update();
  bool isAvailable() const;
  bool isConnected() const;
  bool popEvent(KeyEvent& event);
  int getCandidateCount() const;
  int getSelectedCandidateIndex() const { return selectedCandidateIndex; }
  bool getCandidate(int index, Candidate& candidate) const;
  void selectNextCandidate();
  void selectPreviousCandidate();
  void requestScan();
  bool connectSelectedCandidate();
  void clearCandidates();
  void saveKeyboard(const Candidate& candidate);
  bool matchesSavedKeyboard(const Candidate& candidate) const;
  bool hasSavedKeyboardProfile() const { return hasSavedKeyboard; }
  const char* getSavedKeyboardName() const;
  const char* getStatusText() const;
  uint32_t getStatusVersion() const { return statusVersion; }

 private:
  static constexpr int QUEUE_SIZE = 64;
  static constexpr int MAX_CANDIDATES = 8;
  static constexpr const char* SAVED_KEYBOARD_PATH = "/.crosspoint/keyboard.txt";

  volatile bool started = false;
  volatile bool connected = false;
  volatile bool connecting = false;
  volatile bool scanning = false;
  volatile bool scanRequested = false;
  volatile bool unavailable = false;
  volatile uint16_t devicesSeen = 0;
  volatile uint16_t candidatesSeen = 0;
  volatile uint32_t statusVersion = 0;
  unsigned long lastScanStart = 0;
  mutable char statusText[72] = {};
  Candidate candidates[MAX_CANDIDATES];
  Candidate savedKeyboard;
  Candidate pendingAutoConnect;
  int candidateCount = 0;
  int selectedCandidateIndex = 0;
  bool hasSavedKeyboard = false;
  bool hasPendingAutoConnect = false;
  bool pendingAutoConnectDeferred = false;

  KeyEvent queue[QUEUE_SIZE];
  volatile uint8_t queueHead = 0;
  volatile uint8_t queueTail = 0;
  uint8_t previousKeys[6] = {};
  bool deleteKeyHeld = false;
  bool deleteKeyDidWordDelete = false;
  bool deleteKeyWordModifierHeld = false;
  uint8_t heldDeleteKeyCode = 0;
  unsigned long deleteKeyHoldStartedAt = 0;
  unsigned long nextDeleteWordAt = 0;
  bool cursorArrowKeyHeld = false;
  uint8_t heldCursorArrowKeyCode = 0;
  unsigned long cursorArrowKeyHoldStartedAt = 0;
  unsigned long nextCursorArrowWordAt = 0;

  void loadSavedKeyboard();
  void recordCandidate(const Candidate& candidate);
  void requestAutoConnect(const Candidate& candidate);
  void clearQueue();
  void pushEvent(KeyEvent event);
  void pushTextEvent(const char* text);
  void bumpStatus();
  void handleBootKeyboardReport(const uint8_t* data, size_t length);
  void emitKey(uint8_t keyCode, uint8_t modifiers);
  void updateDeleteKeyHold(bool deleteKeyDown, uint8_t deleteKeyCode, bool deleteWordModifierDown);
  void processDeleteKeyHold();
  void updateCursorArrowKeyHold(bool cursorArrowKeyDown, uint8_t cursorArrowKeyCode);
  void processCursorArrowKeyHold();
  static const char* optionKeyToUtf8(uint8_t keyCode, bool shifted);
  static char keyCodeToAscii(uint8_t keyCode, bool shifted);
  static bool isDeleteKey(uint8_t keyCode);
  static bool isCursorArrowKey(uint8_t keyCode);
  static bool keyAlreadyDown(uint8_t keyCode, const uint8_t* keys);

  friend void bluetoothKeyboardInputHandleReport(const uint8_t* data, size_t length);
  friend void bluetoothKeyboardInputSetConnected(bool value);
  friend void bluetoothKeyboardInputSetConnecting(bool value);
  friend void bluetoothKeyboardInputSetScanning(bool value);
  friend void bluetoothKeyboardInputSetScanRequested(bool value);
  friend void bluetoothKeyboardInputSetUnavailable(bool value);
  friend void bluetoothKeyboardInputRecordDevice(bool candidate);
  friend void bluetoothKeyboardInputRecordCandidate(const Candidate& candidate);
  friend void bluetoothKeyboardInputRequestAutoConnect(const Candidate& candidate);
};

extern BluetoothKeyboardInput BT_KEYBOARD;
