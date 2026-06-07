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

  struct DisplayLine {
    std::string text;
    bool current = false;
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
  int currentNoteIndex = -1;
  bool dirty = false;
  bool saveFailed = false;
  bool creatingNewFile = false;
  unsigned long lastEditAt = 0;
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
  bool handleDocumentControls();
  bool handleNewFilePromptControls();
  void handleNewFilePromptKeyEvent();
  static std::string sanitizeFileName(const std::string& input);
  static bool hasNoteExtension(const char* name);
  static uint32_t modifiedSortKey(const HalFile& file);
  void markDirty();
  bool handlePairingControls();
  void showConnectingNotice();
  void handleKeyEvent();
  void insertChar(char c);
  void insertText(const char* text);
  void insertNewLine();
  void backspace();
  size_t byteCount() const;
  std::vector<DisplayLine> buildDisplayLines(int maxWidth) const;
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
