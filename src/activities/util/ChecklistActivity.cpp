#include "ChecklistActivity.h"

#include <FsHelpers.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "RecentBooksStore.h"
#include "activities/reader/ReaderActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/checklistFilledIcons.h"
#include "components/icons/checklistIcons.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId ROW_ACTION = 1;
using Status = MarkdownChecklist::Status;
}  // namespace

ChecklistActivity::ChecklistActivity(GfxRenderer& renderer, MappedInputManager& input,
                                     std::unique_ptr<MarkdownChecklist> document, Status status,
                                     bool returnToChecklists)
    : Activity("Checklist", renderer, input),
      document_(std::move(document)),
      status_(status),
      returnToChecklists_(returnToChecklists),
      target_(makeUiTarget(renderer)),
      app_(target_, target_.deviceContext()) {}

void ChecklistActivity::onEnter() {
  Activity::onEnter();
  title_ = document_->path().substr(document_->path().find_last_of('/') + 1);
  readable_ = status_ == Status::Ok;
  nav_.reset();
  if (readable_) {
    while (nav_.selected < static_cast<int>(document_->rowCount()) && document_->row(nav_.selected).heading) {
      ++nav_.selected;
    }
    RECENT_BOOKS.addOrUpdateBook(document_->path(), title_, "", "", RecentBook::CoverState::Missing);
  }
  applySharedUiTheme(app_, target_);
  app_.on(ROW_ACTION, &ChecklistActivity::rowEvent, this);
  app_.setScreen(&ChecklistActivity::screen, this);
  requestUpdate();
}

void ChecklistActivity::onExit() {
  document_.reset();
  Activity::onExit();
}

const char* ChecklistActivity::statusText() const {
  switch (status_) {
    case Status::Ok:
      return "";
    case Status::NoTasks:
      return tr(STR_CHECKLIST_EMPTY);
    case Status::ReadError:
      return tr(STR_CHECKLIST_READ_FAILED);
    case Status::LimitExceeded:
      return tr(STR_CHECKLIST_TOO_LARGE);
    case Status::NoMemory:
      return tr(STR_MEMORY_ERROR);
    case Status::SaveError:
      return tr(STR_CHECKLIST_SAVE_FAILED);
    case Status::SourceChanged:
      return tr(STR_CHECKLIST_CHANGED);
    case Status::RecoveryNeeded:
      return tr(STR_CHECKLIST_RECOVERY);
  }
  return tr(STR_ERROR_GENERAL_FAILURE);
}

void ChecklistActivity::activate(int index) {
  if (index < 0 || index > static_cast<int>(document_->rowCount()) + 1) return;
  if (!readable_ || index == static_cast<int>(document_->rowCount()) + 1) {
    auto reader = makeUniqueNoThrow<ReaderActivity>(renderer, mappedInput, document_->path(), false, false, false, true,
                                                    returnToChecklists_);
    if (reader) {
      activityManager.replaceActivity(std::move(reader));
    } else {
      LOG_ERR("CHECKLIST", "Could not allocate text reader");
      RenderLock lock;
      status_ = Status::NoMemory;
      requestUpdate();
    }
  } else if (index == static_cast<int>(document_->rowCount())) {
    auto confirm = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_CHECKLIST_RESET_CONFIRM), "");
    if (!confirm) {
      LOG_ERR("CHECKLIST", "Could not allocate reset confirmation");
      RenderLock lock;
      status_ = Status::NoMemory;
      requestUpdate();
      return;
    }
    startActivityForResult(std::move(confirm), [this](const ActivityResult& result) {
      if (result.isCancelled) return;
      RenderLock lock;
      status_ = document_->reset();
      requestUpdate();
    });
  } else {
    RenderLock lock;
    if (document_->row(index).heading) return;
    nav_.selected = index;
    status_ = document_->toggle(index);
    requestUpdate();
  }
}

void ChecklistActivity::move(int direction, bool page) {
  if (!readable_) return;
  RenderLock lock;
  const int count = static_cast<int>(document_->rowCount()) + 2;
  const int distance = page ? std::min(nav_.pageRowsFor(count), count) : 1;
  int next = (nav_.selected + direction * distance + count) % count;
  while (next < count - 2 && document_->row(next).heading) next = (next + direction + count) % count;
  nav_.selected = next;
  nav_.follow(count);
  requestUpdate();
}

void ChecklistActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (returnToChecklists_) {
      activityManager.goToChecklists(document_->path());
    } else {
      activityManager.goToFileBrowser(FsHelpers::extractFolderPath(document_->path()));
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    int selected;
    {
      RenderLock lock;
      selected = nav_.selected;
    }
    activate(selected);
    return;
  }
  {
    RenderLock lock;
    if (uiReady_) {
      const auto snap = touchSnapshotFrom(mappedInput);
      if (snap.touchPressed || snap.touchReleased) {
        app_.route(snap);
        if (app_.invalidated()) requestUpdate();
      }
    }
  }
  if (pendingRow_ >= 0) {
    const int index = pendingRow_;
    pendingRow_ = -1;
    activate(index);
    return;
  }
  const auto swipe = mappedInput.wasSwipe();
  if (readable_ && (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down)) {
    RenderLock lock;
    const int count = static_cast<int>(document_->rowCount()) + 2;
    const int delta = nav_.pageRowsFor(count) * (swipe == MappedInputManager::SwipeDir::Up ? 1 : -1);
    if (nav_.scrollBy(delta, count)) requestUpdate();
    return;
  }
  navigator_.onNextRelease([this] { move(1, false); });
  navigator_.onPreviousRelease([this] { move(-1, false); });
  navigator_.onNextContinuous([this] { move(1, true); });
  navigator_.onPreviousContinuous([this] { move(-1, true); });
}

void ChecklistActivity::rowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<ChecklistActivity*>(user);
  self->app_.clearTapFlash();
  self->pendingRow_ = event.value;
}

void ChecklistActivity::screen(UiApp::ScreenType& screen, void* user) {
  static_cast<ChecklistActivity*>(user)->buildScreen(screen);
}

void ChecklistActivity::buildScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const auto header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(std::max(top, header.y + header.height) + metrics.verticalSpacing),
                  static_cast<int16_t>(right), static_cast<int16_t>(bottom + metrics.buttonHintsHeight),
                  static_cast<int16_t>(left)});
  if (status_ != Status::Ok) {
    auto textStyle = screen.theme().smallText;
    textStyle.maxLines = 4;
    const auto measured = fui::measureWrappedText(screen.target(), statusText(), textStyle, screen.body().width);
    screen.target().text(screen.takeTop(measured.height), statusText(), textStyle);
    screen.spacer(metrics.verticalSpacing);
  }
  if (!readable_) {
    fui::ListItem item;
    item.label = tr(STR_CHECKLIST_VIEW_TEXT);
    fui::ListProps props;
    props.items = &item;
    props.count = 1;
    props.selectedIndex = 0;
    props.action = ROW_ACTION;
    props.inputMask = fui::InputTouch;
    screen.list(props);
    return;
  }
  char progress[80];
  snprintf(progress, sizeof(progress), tr(STR_CHECKLIST_PROGRESS), static_cast<unsigned>(document_->completedCount()),
           static_cast<unsigned>(document_->taskCount()));
  screen.target().text(screen.takeTop(screen.target().lineHeight(screen.theme().smallText.font)), progress,
                       screen.theme().smallText);
  screen.spacer(metrics.verticalSpacing);
  fui::ListProps props;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 8;
  props.iconSize = 24;
  props.balanceWrappedLabelWithValue = false;
  props.valueInset = 8;
  props.action = ROW_ACTION;
  props.inputMask = fui::InputTouch;
  const int count = static_cast<int>(document_->rowCount()) + 2;
  configureUiList(props, screen.theme(), screen.body());
  // Checklist sentences must wrap on button devices too.
  props.labelText.maxLines = 8;
  props.rowHeight = std::max(props.rowHeight, screen.theme().rowHeight);
  nav_.syncToProps(screen.body(), props.rowHeight, props.rowGap, count, props);
  const size_t window = std::min(WINDOW_ROWS, static_cast<size_t>(count - nav_.top));
  for (size_t i = 0; i < window; ++i) {
    auto& item = items_[i];
    item = {};
    const size_t index = nav_.top + i;
    item.actionValue = static_cast<int16_t>(index);
    if (index >= document_->rowCount()) {
      item.label = index == document_->rowCount() ? tr(STR_CHECKLIST_RESET) : tr(STR_CHECKLIST_VIEW_TEXT);
    } else {
      const auto& row = document_->row(index);
      item.label = document_->label(index);
      item.isHeader = row.heading;
      item.enabled = !row.heading;
      if (!row.heading) {
        item.icon = fui::bitmapFromIcon(row.checked() ? icon_checklist_complete_24 : icon_checklist_empty_24);
        item.value = row.checked() ? tr(STR_DONE) : nullptr;
      }
    }
  }
  props.items = items_;
  props.itemsWindowFirst = nav_.top;
  props.itemsWindowCount = window;
  props.count = count;
  screen.list(props);
}

void ChecklistActivity::render(RenderLock&&) {
  uiReady_ = false;
  // Wrapped tasks have variable height; rebuild until the SDK confirms that
  // the button-selected row is actually in the drawn viewport.
  for (size_t attempt = 0; attempt <= document_->rowCount() + 1; ++attempt) {
    renderer.clearScreen();
    TouchHeaderBackButton::draw(renderer, target_, TouchHeaderBackButton::headerRect(renderer, mappedInput),
                                title_.c_str(), false);
    app_.render();
    if (!nav_.consumeRebuildNeeded()) break;
  }
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT),
                                            readable_ ? tr(STR_DIR_UP) : "", readable_ ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  uiReady_ = true;
  renderer.displayBuffer();
}
