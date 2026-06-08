#include "TextEditorActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "Utf8.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "input/BluetoothKeyboardInput.h"

namespace {
constexpr int EDITOR_FONT_ID = NOTOSERIF_16_FONT_ID;
constexpr int CANDIDATE_TITLE_FONT_ID = UI_12_FONT_ID;
constexpr int CANDIDATE_FONT_ID = UI_12_FONT_ID;
constexpr int STATUS_FONT_ID = SMALL_FONT_ID;
constexpr int EDITOR_MARGIN_X = 22;
constexpr int EDITOR_MARGIN_TOP = 26;
constexpr int EDITOR_MARGIN_BOTTOM = 40;
constexpr int LINE_GAP = 4;
constexpr int MAX_VISIBLE_CANDIDATES = 5;

bool isSentenceTerminator(const char c) { return c == '.' || c == '!' || c == '?'; }
}  // namespace

void TextEditorActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  loadInitialDocument();
  BT_KEYBOARD.begin();
  lastKeyboardStatusVersion = BT_KEYBOARD.getStatusVersion();
  requestUpdate();
}

void TextEditorActivity::onExit() {
  saveCurrentDocument();
  Activity::onExit();
}

void TextEditorActivity::loop() {
  if (!creatingNewFile && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  const bool wasConnected = BT_KEYBOARD.isConnected();
  BT_KEYBOARD.update();
  const uint32_t keyboardStatusVersion = BT_KEYBOARD.getStatusVersion();
  if (BT_KEYBOARD.isConnected() != wasConnected || keyboardStatusVersion != lastKeyboardStatusVersion) {
    lastKeyboardStatusVersion = keyboardStatusVersion;
    requestUpdate();
  }

  if (creatingNewFile) {
    handleNewFilePromptControls();
    handleNewFilePromptKeyEvent();
  } else if (!handlePairingControls() && !handleDocumentControls()) {
    handleKeyEvent();
  }

  if (dirty && millis() - lastEditAt >= AUTOSAVE_DELAY_MS) {
    saveCurrentDocument();
    requestUpdate();
  }
}

void TextEditorActivity::render(RenderLock&&) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int contentWidth = pageWidth - EDITOR_MARGIN_X * 2;
  const int lineHeight = renderer.getLineHeight(EDITOR_FONT_ID) + LINE_GAP;
  const int statusHeight = renderer.getLineHeight(STATUS_FONT_ID);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int hintsHeight = !BT_KEYBOARD.isConnected() ? metrics.buttonHintsHeight : 0;
  const int statusY = pageHeight - hintsHeight - statusHeight - 9;
  const int bottomY = statusY - EDITOR_MARGIN_BOTTOM + statusHeight - lineHeight;
  const int candidateCount = BT_KEYBOARD.getCandidateCount();
  const bool showCandidatePicker =
      !BT_KEYBOARD.isConnected() && !BT_KEYBOARD.hasSavedKeyboardProfile() && candidateCount > 0;
  const int visibleCandidateCount = std::min(candidateCount, MAX_VISIBLE_CANDIDATES);
  const int pickerHeight = showCandidatePicker
                               ? (renderer.getLineHeight(CANDIDATE_TITLE_FONT_ID) + 10 +
                                  visibleCandidateCount * (renderer.getLineHeight(CANDIDATE_FONT_ID) + 8) + 8)
                               : 0;

  renderer.clearScreen();

  const std::vector<DisplayLine> displayLines = buildDisplayLines(contentWidth);
  int y = bottomY;
  for (auto it = displayLines.rbegin(); it != displayLines.rend() && y >= EDITOR_MARGIN_TOP + pickerHeight; ++it) {
    drawDisplayLine(EDITOR_FONT_ID, EDITOR_MARGIN_X, y, *it);
    y -= lineHeight;
  }

  if (showCandidatePicker) {
    renderCandidatePicker(EDITOR_MARGIN_X, EDITOR_MARGIN_TOP, contentWidth);
  }

  if (!BT_KEYBOARD.isConnected()) {
    const std::string visibleStatus = renderer.truncatedText(STATUS_FONT_ID, BT_KEYBOARD.getStatusText(), contentWidth);
    renderer.drawText(STATUS_FONT_ID, EDITOR_MARGIN_X, statusY, visibleStatus.c_str(), true);
  } else if (!currentNoteName.empty()) {
    const std::string visibleName = renderer.truncatedText(STATUS_FONT_ID, currentNoteName.c_str(), contentWidth);
    drawGrayTextLine(STATUS_FONT_ID, EDITOR_MARGIN_X, statusY, visibleName);
  }

  if (dirty) {
    constexpr const char* UNSAVED_MARKER = "*";
    const int markerWidth = renderer.getTextWidth(STATUS_FONT_ID, UNSAVED_MARKER);
    renderer.drawText(STATUS_FONT_ID, pageWidth - EDITOR_MARGIN_X - markerWidth, statusY, UNSAVED_MARKER, true);
  }

  if (creatingNewFile) {
    renderNewFilePrompt(pageWidth, pageHeight);
  }

  if (creatingNewFile) {
    const auto labels = mappedInput.mapLabels("Cancel", "Save", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (!BT_KEYBOARD.isConnected()) {
    const auto labels = mappedInput.mapLabels("Back", candidateCount > 0 ? "Connect" : "Scan",
                                              candidateCount > 1 ? "Up" : "", candidateCount > 1 ? "Down" : "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

void TextEditorActivity::loadInitialDocument() {
  Storage.ensureDirectoryExists(NOTES_DIR);
  loadNotes();
  createDefaultNoteIfNeeded();
  loadNotes();

  if (!noteFiles.empty()) {
    openNoteAt(0);
  }
}

void TextEditorActivity::loadCurrentDocument() {
  lines.clear();
  std::string text;
  if (!currentNotePath.empty() && Storage.exists(currentNotePath.c_str())) {
    String stored = Storage.readFile(currentNotePath.c_str());
    text.assign(stored.c_str(), stored.length());
    if (text.size() > MAX_TEXT_BYTES) {
      text.resize(MAX_TEXT_BYTES);
    }
  }

  std::string current;
  for (char c : text) {
    if (c == '\r') continue;
    if (c == '\n') {
      lines.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  lines.push_back(current);

  dirty = false;
  saveFailed = false;
}

void TextEditorActivity::saveCurrentDocument() {
  if (!dirty || currentNotePath.empty()) return;

  Storage.ensureDirectoryExists(NOTES_DIR);
  std::string text;
  text.reserve(std::min(byteCount() + lines.size(), MAX_TEXT_BYTES));
  for (size_t i = 0; i < lines.size(); i++) {
    if (i > 0) text.push_back('\n');
    text += lines[i];
  }

  saveFailed = !Storage.writeFile(currentNotePath.c_str(), String(text.c_str()));
  if (!saveFailed) {
    dirty = false;
    loadNotes();
  }
}

void TextEditorActivity::loadNotes() {
  noteFiles.clear();
  auto root = Storage.open(NOTES_DIR);
  if (!root || !root.isDirectory()) {
    return;
  }

  root.rewindDirectory();
  char nameBuffer[96] = {};
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    memset(nameBuffer, 0, sizeof(nameBuffer));
    file.getName(nameBuffer, sizeof(nameBuffer));
    if (nameBuffer[0] == '\0' || nameBuffer[0] == '.' || file.isDirectory() || !hasNoteExtension(nameBuffer)) {
      continue;
    }

    NoteFile note;
    note.name = nameBuffer;
    note.path = std::string(NOTES_DIR) + "/" + note.name;
    note.modified = modifiedSortKey(file);
    noteFiles.push_back(note);
  }
  root.close();

  std::sort(noteFiles.begin(), noteFiles.end(), [](const NoteFile& a, const NoteFile& b) {
    if (a.modified != b.modified) return a.modified > b.modified;
    return a.name < b.name;
  });

  currentNoteIndex = -1;
  for (size_t i = 0; i < noteFiles.size(); i++) {
    if (noteFiles[i].path == currentNotePath) {
      currentNoteIndex = static_cast<int>(i);
      currentNoteName = noteFiles[i].name;
      break;
    }
  }
}

void TextEditorActivity::createDefaultNoteIfNeeded() {
  if (!noteFiles.empty()) return;

  const std::string defaultPath = std::string(NOTES_DIR) + "/" + DEFAULT_NOTE_NAME;
  if (!Storage.exists(defaultPath.c_str())) {
    Storage.writeFile(defaultPath.c_str(), String(""));
  }
}

void TextEditorActivity::openNoteAt(const int index) {
  if (noteFiles.empty()) return;

  const int count = static_cast<int>(noteFiles.size());
  const int wrappedIndex = (index % count + count) % count;
  currentNoteIndex = wrappedIndex;
  currentNotePath = noteFiles[wrappedIndex].path;
  currentNoteName = noteFiles[wrappedIndex].name;
  loadCurrentDocument();
  requestUpdate();
}

void TextEditorActivity::openNotePath(const std::string& path) {
  currentNotePath = path;
  loadNotes();
  if (currentNoteIndex < 0) {
    currentNoteName = path.substr(path.find_last_of('/') + 1);
  }
  loadCurrentDocument();
  requestUpdate();
}

void TextEditorActivity::openAdjacentNote(const int direction) {
  saveCurrentDocument();
  loadNotes();
  if (noteFiles.empty()) return;
  if (currentNoteIndex < 0) currentNoteIndex = 0;
  openNoteAt(currentNoteIndex + direction);
}

void TextEditorActivity::startNewFilePrompt() {
  saveCurrentDocument();
  pendingFileName.clear();
  creatingNewFile = true;
  requestUpdate();
}

void TextEditorActivity::cancelNewFilePrompt() {
  creatingNewFile = false;
  pendingFileName.clear();
  requestUpdate();
}

void TextEditorActivity::confirmNewFilePrompt() {
  std::string fileName = sanitizeFileName(pendingFileName);
  if (fileName.empty()) {
    fileName = "Untitled";
  }
  if (!hasNoteExtension(fileName.c_str())) {
    fileName += ".txt";
  }

  Storage.ensureDirectoryExists(NOTES_DIR);
  std::string path = std::string(NOTES_DIR) + "/" + fileName;
  if (Storage.exists(path.c_str())) {
    const size_t extensionStart = fileName.rfind('.');
    const std::string stem = extensionStart == std::string::npos ? fileName : fileName.substr(0, extensionStart);
    const std::string extension = extensionStart == std::string::npos ? ".txt" : fileName.substr(extensionStart);
    for (int suffix = 2; suffix < 1000 && Storage.exists(path.c_str()); suffix++) {
      fileName = stem + " " + std::to_string(suffix) + extension;
      path = std::string(NOTES_DIR) + "/" + fileName;
    }
  }

  saveFailed = !Storage.writeFile(path.c_str(), String(""));
  if (saveFailed) {
    requestUpdate();
    return;
  }

  creatingNewFile = false;
  pendingFileName.clear();
  openNotePath(path);
}

bool TextEditorActivity::handleDocumentControls() {
  if (mappedInput.isPressed(MappedInputManager::Button::Down) && mappedInput.getHeldTime() >= SIDE_BUTTON_HOLD_MS) {
    startNewFilePrompt();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down) && mappedInput.getHeldTime() < SIDE_BUTTON_HOLD_MS) {
    openAdjacentNote(1);
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up) && mappedInput.getHeldTime() < SIDE_BUTTON_HOLD_MS) {
    openAdjacentNote(-1);
    return true;
  }

  return false;
}

bool TextEditorActivity::handleNewFilePromptControls() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancelNewFilePrompt();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirmNewFilePrompt();
    return true;
  }

  return false;
}

void TextEditorActivity::handleNewFilePromptKeyEvent() {
  bool changed = false;
  BluetoothKeyboardInput::KeyEvent event;
  while (BT_KEYBOARD.popEvent(event)) {
    switch (event.type) {
      case BluetoothKeyboardInput::KeyType::Character:
        if (event.text[0] != '\0') {
          if (pendingFileName.size() + std::strlen(event.text) <= 64) {
            pendingFileName += event.text;
            changed = true;
          }
        } else if (pendingFileName.size() < 64) {
          pendingFileName.push_back(event.character);
          changed = true;
        }
        break;
      case BluetoothKeyboardInput::KeyType::Tab:
        if (pendingFileName.size() < 64) {
          pendingFileName.push_back(' ');
          changed = true;
        }
        break;
      case BluetoothKeyboardInput::KeyType::Enter:
        confirmNewFilePrompt();
        return;
      case BluetoothKeyboardInput::KeyType::Backspace:
      case BluetoothKeyboardInput::KeyType::DeleteKey:
        if (!pendingFileName.empty()) {
          utf8RemoveLastChar(pendingFileName);
          changed = true;
        }
        break;
      case BluetoothKeyboardInput::KeyType::DeleteWord:
        while (!pendingFileName.empty() && std::isspace(static_cast<unsigned char>(pendingFileName.back()))) {
          utf8RemoveLastChar(pendingFileName);
          changed = true;
        }
        while (!pendingFileName.empty() && !std::isspace(static_cast<unsigned char>(pendingFileName.back()))) {
          utf8RemoveLastChar(pendingFileName);
          changed = true;
        }
        break;
      case BluetoothKeyboardInput::KeyType::Escape:
        cancelNewFilePrompt();
        return;
      default:
        break;
    }
  }

  if (changed) requestUpdate();
}

std::string TextEditorActivity::sanitizeFileName(const std::string& input) {
  std::string result;
  bool previousWasSpace = false;
  for (const char c : input) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (uc < 32) continue;
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      if (!result.empty() && !previousWasSpace) {
        result.push_back(' ');
        previousWasSpace = true;
      }
      continue;
    }
    if (std::isspace(uc)) {
      if (!result.empty() && !previousWasSpace) {
        result.push_back(' ');
        previousWasSpace = true;
      }
      continue;
    }
    result.push_back(c);
    previousWasSpace = false;
  }

  while (!result.empty() && (result.back() == ' ' || result.back() == '.')) {
    result.pop_back();
  }
  if (result.size() > 64) {
    result.resize(64);
  }
  return result;
}

bool TextEditorActivity::hasNoteExtension(const char* name) {
  if (!name) return false;
  const char* extension = std::strrchr(name, '.');
  if (!extension) return false;
  std::string value = extension;
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value == ".txt" || value == ".md" || value == ".markdown";
}

uint32_t TextEditorActivity::modifiedSortKey(const HalFile& file) {
  uint16_t date = 0;
  uint16_t time = 0;
  if (!file.getModifyDateTime(&date, &time)) {
    return 0;
  }
  return (static_cast<uint32_t>(date) << 16) | time;
}

size_t TextEditorActivity::currentSentenceStart(const std::string& line) {
  size_t start = 0;

  for (size_t terminator = 0; terminator < line.size();) {
    if (!isSentenceTerminator(line[terminator])) {
      terminator++;
      continue;
    }

    size_t punctuationEnd = terminator + 1;
    while (punctuationEnd < line.size() && isSentenceTerminator(line[punctuationEnd])) {
      punctuationEnd++;
    }

    size_t nextSentence = punctuationEnd;
    while (nextSentence < line.size() && std::isspace(static_cast<unsigned char>(line[nextSentence]))) {
      nextSentence++;
    }
    if (nextSentence < line.size()) {
      start = punctuationEnd;
    }

    terminator = punctuationEnd;
  }
  return start;
}

void TextEditorActivity::markDirty() {
  dirty = true;
  saveFailed = false;
  lastEditAt = millis();
}

bool TextEditorActivity::handlePairingControls() {
  if (BT_KEYBOARD.isConnected()) return false;

  const int candidateCount = BT_KEYBOARD.getCandidateCount();
  if (candidateCount > 0) {
    const bool previousPressed = mappedInput.wasPressed(MappedInputManager::Button::Left) ||
                                 mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                                 mappedInput.wasPressed(MappedInputManager::Button::PageBack);
    const bool nextPressed = mappedInput.wasPressed(MappedInputManager::Button::Right) ||
                             mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                             mappedInput.wasPressed(MappedInputManager::Button::PageForward);
    const bool connectPressed = mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
                                mappedInput.wasReleased(MappedInputManager::Button::Confirm);

    if (connectPressed) {
      LOG_DBG("BTK", "Pairing control: connect selected candidate");
      showConnectingNotice();
      if (BT_KEYBOARD.connectSelectedCandidate()) {
        requestUpdateAndWait();
      } else {
        requestUpdate();
      }
      return true;
    }

    if (previousPressed) {
      LOG_DBG("BTK", "Pairing control: previous candidate");
      BT_KEYBOARD.selectPreviousCandidate();
      requestUpdate();
      return true;
    }
    if (nextPressed) {
      LOG_DBG("BTK", "Pairing control: next candidate");
      BT_KEYBOARD.selectNextCandidate();
      requestUpdate();
      return true;
    }
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
             mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasAnyReleased()) {
    LOG_DBG("BTK", "Pairing control: request scan");
    BT_KEYBOARD.requestScan();
    requestUpdate();
    return true;
  }

  return false;
}

void TextEditorActivity::showConnectingNotice() {
  RenderLock lock(*this);
  GUI.drawPopup(renderer, "Connecting...");
  renderer.displayBuffer();
}

void TextEditorActivity::handleKeyEvent() {
  bool changed = false;
  BluetoothKeyboardInput::KeyEvent event;
  while (BT_KEYBOARD.popEvent(event)) {
    switch (event.type) {
      case BluetoothKeyboardInput::KeyType::Character:
        if (event.text[0] != '\0') {
          insertText(event.text);
        } else {
          insertChar(event.character);
        }
        changed = true;
        break;
      case BluetoothKeyboardInput::KeyType::Tab:
        insertText("  ");
        changed = true;
        break;
      case BluetoothKeyboardInput::KeyType::Enter:
        insertNewLine();
        changed = true;
        break;
      case BluetoothKeyboardInput::KeyType::Backspace:
      case BluetoothKeyboardInput::KeyType::DeleteKey:
        backspace();
        changed = true;
        break;
      case BluetoothKeyboardInput::KeyType::DeleteWord:
        deleteWord();
        changed = true;
        break;
      case BluetoothKeyboardInput::KeyType::Escape:
        onGoHome();
        return;
      default:
        break;
    }
  }

  if (changed) requestUpdate();
}

void TextEditorActivity::insertChar(const char c) {
  if (c == '\0' || byteCount() >= MAX_TEXT_BYTES) return;
  if (lines.empty()) lines.push_back("");
  lines.back().push_back(c);
  markDirty();
}

void TextEditorActivity::insertText(const char* text) {
  if (!text) return;
  const size_t currentBytes = byteCount();
  if (currentBytes >= MAX_TEXT_BYTES) return;

  if (lines.empty()) lines.push_back("");
  const size_t available = MAX_TEXT_BYTES - currentBytes;
  const size_t requested = std::min(std::strlen(text), available);
  const int safeLength = utf8SafeTruncateBuffer(text, static_cast<int>(requested));
  if (safeLength <= 0) {
    return;
  }
  lines.back().append(text, static_cast<size_t>(safeLength));
  markDirty();
}

void TextEditorActivity::insertNewLine() {
  if (byteCount() >= MAX_TEXT_BYTES) return;
  lines.push_back("");
  markDirty();
}

void TextEditorActivity::backspace() {
  if (lines.empty()) {
    lines.push_back("");
    return;
  }
  if (!lines.back().empty()) {
    utf8RemoveLastChar(lines.back());
    markDirty();
    return;
  }
  if (lines.size() > 1) {
    lines.pop_back();
    markDirty();
  }
}

void TextEditorActivity::deleteWord() {
  if (lines.empty()) {
    lines.push_back("");
    return;
  }

  std::string& line = lines.back();
  if (line.empty()) {
    if (lines.size() > 1) {
      lines.pop_back();
      markDirty();
    }
    return;
  }

  bool removed = false;
  while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
    utf8RemoveLastChar(line);
    removed = true;
  }

  while (!line.empty() && !std::isspace(static_cast<unsigned char>(line.back()))) {
    utf8RemoveLastChar(line);
    removed = true;
  }

  if (removed) {
    markDirty();
  }
}

size_t TextEditorActivity::byteCount() const {
  size_t total = lines.empty() ? 0 : lines.size() - 1;
  for (const auto& line : lines) total += line.size();
  return total;
}

std::vector<TextEditorActivity::DisplayLine> TextEditorActivity::buildDisplayLines(const int maxWidth) const {
  std::vector<DisplayLine> displayLines;
  if (lines.empty()) {
    displayLines.push_back(DisplayLine{"", 0});
    return displayLines;
  }

  const auto appendWrapped = [&](const std::string& text, const size_t currentStart) {
    const auto wrapped = renderer.wrappedText(EDITOR_FONT_ID, text.c_str(), maxWidth, 16);
    if (wrapped.empty()) {
      displayLines.push_back(DisplayLine{"", currentStart == std::string::npos ? std::string::npos : 0});
      return;
    }

    size_t searchFrom = 0;
    for (const auto& wrappedLine : wrapped) {
      size_t lineStart = text.find(wrappedLine, searchFrom);
      if (lineStart == std::string::npos) {
        lineStart = searchFrom;
      }
      const size_t lineEnd = lineStart + wrappedLine.size();

      size_t lineCurrentStart = std::string::npos;
      if (currentStart != std::string::npos) {
        if (currentStart <= lineStart) {
          lineCurrentStart = 0;
        } else if (currentStart < lineEnd) {
          lineCurrentStart = currentStart - lineStart;
        }
      }

      displayLines.push_back(DisplayLine{wrappedLine, lineCurrentStart});
      searchFrom = lineEnd;
      while (searchFrom < text.size() && text[searchFrom] == ' ') {
        searchFrom++;
      }
    }
  };

  for (size_t i = 0; i < lines.size(); i++) {
    const bool isCurrent = i == lines.size() - 1;
    if (!isCurrent) {
      appendWrapped(lines[i], std::string::npos);
      continue;
    }

    appendWrapped(lines[i], currentSentenceStart(lines[i]));
  }
  return displayLines;
}

void TextEditorActivity::drawDisplayLine(const int fontId, const int x, const int y, const DisplayLine& line) const {
  const std::string text = line.text.empty() ? " " : line.text;
  if (line.currentStart == std::string::npos) {
    drawGrayTextLine(fontId, x, y, text);
    return;
  }

  if (line.currentStart >= text.size()) {
    drawGrayTextLine(fontId, x, y, text);
    return;
  }

  if (line.currentStart == 0) {
    renderer.drawText(fontId, x, y, text.c_str(), true);
    return;
  }

  const std::string prefix = text.substr(0, line.currentStart);
  renderer.drawText(fontId, x, y, text.c_str(), true);
  drawGrayTextLine(fontId, x, y, prefix);
}

int TextEditorActivity::renderCandidatePicker(const int x, const int y, const int maxWidth) {
  const int rowHeight = renderer.getLineHeight(CANDIDATE_FONT_ID) + 8;
  const int candidateCount = BT_KEYBOARD.getCandidateCount();
  const int selectedIndex = BT_KEYBOARD.getSelectedCandidateIndex();
  const int visibleCount = std::min(candidateCount, MAX_VISIBLE_CANDIDATES);
  int firstIndex = selectedIndex - visibleCount / 2;
  firstIndex = std::max(0, std::min(firstIndex, candidateCount - visibleCount));

  renderer.drawText(CANDIDATE_TITLE_FONT_ID, x, y, "Keyboards", true, EpdFontFamily::BOLD);
  int rowY = y + renderer.getLineHeight(CANDIDATE_TITLE_FONT_ID) + 10;
  for (int row = 0; row < visibleCount; row++) {
    BluetoothKeyboardInput::Candidate candidate;
    const int candidateIndex = firstIndex + row;
    if (!BT_KEYBOARD.getCandidate(candidateIndex, candidate)) continue;

    std::string label = candidate.name[0] == '\0' ? "Unnamed keyboard" : candidate.name;
    label += " ";
    label += std::to_string(candidate.rssi);

    label = renderer.truncatedText(CANDIDATE_FONT_ID, label.c_str(), maxWidth);
    if (candidateIndex == selectedIndex) {
      drawGrayBackground(x - 4, rowY - 4, maxWidth + 8, rowHeight);
      renderer.drawText(CANDIDATE_FONT_ID, x, rowY, label.c_str(), true);
    } else {
      drawGrayTextLine(CANDIDATE_FONT_ID, x, rowY, label);
    }
    rowY += rowHeight;
  }
  return rowY - y;
}

void TextEditorActivity::renderNewFilePrompt(const int pageWidth, const int pageHeight) {
  const int panelWidth = std::min(pageWidth - EDITOR_MARGIN_X * 2, 360);
  const int panelHeight = 92;
  const int x = (pageWidth - panelWidth) / 2;
  const int y = std::max(EDITOR_MARGIN_TOP, (pageHeight - panelHeight) / 3);
  const int inputY = y + 42;
  const int inputHeight = renderer.getLineHeight(UI_12_FONT_ID) + 12;

  renderer.fillRect(x - 2, y - 2, panelWidth + 4, panelHeight + 4, true);
  renderer.fillRect(x, y, panelWidth, panelHeight, false);
  renderer.drawRect(x, y, panelWidth, panelHeight, true);
  renderer.drawText(UI_12_FONT_ID, x + 12, y + 12, "New note", true, EpdFontFamily::BOLD);

  renderer.drawRect(x + 12, inputY, panelWidth - 24, inputHeight, true);
  std::string visibleName = pendingFileName.empty() ? " " : pendingFileName;
  visibleName = renderer.truncatedText(UI_12_FONT_ID, visibleName.c_str(), panelWidth - 36);
  renderer.drawText(UI_12_FONT_ID, x + 18, inputY + 6, visibleName.c_str(), true);

  const int cursorX = x + 18 + renderer.getTextWidth(UI_12_FONT_ID, visibleName.c_str()) + 2;
  if (cursorX < x + panelWidth - 18) {
    renderer.drawLine(cursorX, inputY + 5, cursorX, inputY + inputHeight - 6, true);
  }
}

void TextEditorActivity::drawGrayBackground(const int x, const int y, const int width, const int height) const {
  for (int yy = y; yy < y + height; yy++) {
    for (int xx = x; xx < x + width; xx++) {
      if (xx % 2 == 0 && yy % 2 == 0) renderer.drawPixel(xx, yy, true);
    }
  }
}

void TextEditorActivity::drawGrayTextLine(const int fontId, const int x, const int y, const std::string& text) const {
  renderer.drawText(fontId, x, y, text.c_str(), true);
  const int height = renderer.getLineHeight(fontId);
  const int width = std::min(renderer.getTextWidth(fontId, text.c_str()) + 2, renderer.getScreenWidth() - x);
  for (int yy = y; yy < y + height; yy++) {
    for (int xx = x; xx < x + width; xx++) {
      if ((xx + yy) % 2 != 0) renderer.drawPixel(xx, yy, false);
    }
  }
}
