#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/MarkdownChecklist.h"

class ChecklistActivity final : public Activity {
 public:
  ChecklistActivity(GfxRenderer& renderer, MappedInputManager& input, std::unique_ptr<MarkdownChecklist> document,
                    MarkdownChecklist::Status status, bool returnToChecklists = false);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  std::string getCurrentBookPath() const override { return document_->path(); }

 private:
  using UiApp = freeink::ui::FreeInkApp<32, 2>;
  static constexpr size_t WINDOW_ROWS = 32;
  std::unique_ptr<MarkdownChecklist> document_;
  MarkdownChecklist::Status status_;
  bool readable_ = false;
  bool returnToChecklists_ = false;
  std::string title_;
  freeink::ui::GfxRendererTarget target_;
  UiApp app_;
  freeink::ui::ListNav nav_;
  // Reused window (~3 KiB on device), inside the heap-owned activity rather
  // than a render-task stack or a per-frame vector allocation.
  freeink::ui::ListItem items_[WINDOW_ROWS];
  ButtonNavigator navigator_;
  bool uiReady_ = false;
  int pendingRow_ = -1;

  static void screen(UiApp::ScreenType& screen, void* user);
  static void rowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildScreen(UiApp::ScreenType& screen);
  void activate(int index);
  void move(int direction, bool page);
  const char* statusText() const;
};
