#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "activities/Activity.h"

class HalFile;

class TextEditorActivity final : public Activity {
  static constexpr const char* NOTES_DIR = "/notes";
  static constexpr const char* DEFAULT_NOTE_NAME = "Untitled.txt";
  static constexpr size_t MAX_TEXT_BYTES = 64 * 1024;
  static constexpr unsigned long AUTOSAVE_DELAY_MS = 2000;
  static constexpr unsigned long SIDE_BUTTON_HOLD_MS = 700;
  static constexpr unsigned long CURSOR_WORD_HOLD_MS = 1000;
  static constexpr unsigned long CURSOR_WORD_REPEAT_MS = 1000;

  struct DisplayLine {
    std::string text;
    size_t sourceLineIndex = std::string::npos;
    size_t sourceStart = 0;
    size_t currentStart = std::string::npos;
    size_t currentEnd = std::string::npos;
    size_t cursorOffset = std::string::npos;
  };

  struct SentenceRange {
    size_t start = 0;
    size_t end = std::string::npos;
  };

  struct NoteFile {
    std::string name;
    std::string path;
    uint32_t modified = 0;
  };

  std::vector<std::string> lines;
  std::vector<NoteFile> noteFiles;
  std::string currentNotePath;
  std::string currentNoteName;
  std::string pendingFileName;
  size_t cursorLineIndex = 0;
  size_t cursorByteIndex = 0;
  int currentNoteIndex = -1;
  bool dirty = false;
  bool saveFailed = false;
  bool creatingNewFile = false;
  bool deleteFileHoldConsumed = false;
  int cursorWordHoldDirection = 0;
  unsigned long lastEditAt = 0;
  unsigned long nextCursorWordMoveAt = 0;
  uint32_t lastKeyboardStatusVersion = 0;

  void loadInitialDocument();
  void loadCurrentDocument();
  void saveCurrentDocument();
  void loadNotes();
  void createDefaultNoteIfNeeded();
  void openNoteAt(int index);
  void openNotePath(const std::string& path);
  void openAdjacentNote(int direction);
  void startNewFilePrompt();
  void cancelNewFilePrompt();
  void confirmNewFilePrompt();
  void startDeleteFileConfirmation();
  void deleteCurrentFile();
  bool handleDocumentControls();
  bool handleNewFilePromptControls();
  void handleNewFilePromptKeyEvent();
  static std::string sanitizeFileName(const std::string& input);
  static bool hasNoteExtension(const char* name);
  static uint32_t modifiedSortKey(const HalFile& file);
  static SentenceRange currentSentenceRange(const std::string& line, size_t cursor);
  static bool isUtf8Continuation(unsigned char c);
  static size_t previousUtf8Boundary(const std::string& text, size_t index);
  static size_t nextUtf8Boundary(const std::string& text, size_t index);
  static size_t clampUtf8Boundary(const std::string& text, size_t index);
  void setCursorToEnd();
  void ensureCursorValid();
  void markDirty();
  bool handlePairingControls();
  void showConnectingNotice();
  void handleKeyEvent();
  bool handleCursorControls();
  void moveCursorLeft();
  void moveCursorRight();
  void moveCursorWordLeft();
  void moveCursorWordRight();
  void moveCursorVertical(int direction);
  void insertChar(char c);
  void insertText(const char* text);
  void insertNewLine();
  void backspace();
  void deleteForward();
  void deleteWord();
  void deleteForwardWord();
  size_t byteCount() const;
  std::vector<DisplayLine> buildDisplayLines(int maxWidth) const;
  int textCursorWidth(int fontId, const std::string& text) const;
  void drawDisplayLine(int fontId, int x, int y, const DisplayLine& line) const;
  int renderCandidatePicker(int x, int y, int maxWidth);
  void renderNewFilePrompt(int pageWidth, int pageHeight);
  void drawGrayBackground(int x, int y, int width, int height) const;
  void drawGrayTextLine(int fontId, int x, int y, const std::string& text) const;

 public:
  explicit TextEditorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TextEditor", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return true; }
  bool preventAutoSleep() override { return true; }
};
