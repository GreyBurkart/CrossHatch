#include "RecentBooksActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <TrashPaths.h>

#include <algorithm>
#include <memory>

#include "BookActions.h"
#include "FileBrowserActionActivity.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/reader/EpubReaderActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/CompactHeader.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr size_t MAX_LIST_RECENT_BOOKS = VirtualViews::MAX_BOOKS;
// Hold threshold for the long-press action menu (firmware convention).
constexpr unsigned long LONG_PRESS_MS = 1000;
constexpr unsigned long ACTION_FEEDBACK_MS = 1000;
constexpr fui::ActionId ACTION_ROW = 1;
}  // namespace

RecentBooksActivity::RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const ViewMode view)
    : Activity("RecentBooks", renderer, mappedInput),
      viewMode(view),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void RecentBooksActivity::loadRecentBooks() {
  recentBooks.clear();
  scanStatus = {};
  if (viewMode == ViewMode::RecentlyAdded) {
    scanStatus = VirtualViews::loadRecentlyAdded(recentBooks);
    return;
  }
  if (viewMode == ViewMode::RecentlyFinished) {
    scanStatus = VirtualViews::loadRecentlyFinished(recentBooks);
    return;
  }
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(books.size(), MAX_LIST_RECENT_BOOKS));

  for (const auto& book : books) {
    if (recentBooks.size() >= MAX_LIST_RECENT_BOOKS) {
      break;
    }
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }
    recentBooks.push_back(book);
  }
}

const char* RecentBooksActivity::viewTitle() const {
  switch (viewMode) {
    case ViewMode::RecentlyAdded:
      return tr(STR_RECENTLY_ADDED);
    case ViewMode::RecentlyFinished:
      return tr(STR_RECENTLY_FINISHED);
    default:
      return tr(STR_RECENTLY_OPENED);
  }
}

void RecentBooksActivity::showViewSelector() {
  // One short-lived foreground selection screen; no reader or scan is retained.
  auto selector = makeUniqueNoThrow<OptionSelectionActivity>(
      renderer, mappedInput, "LibraryViewSelect", StrId::STR_LIBRARY_VIEW,
      std::vector<std::string>{tr(STR_RECENTLY_OPENED), tr(STR_RECENTLY_ADDED), tr(STR_RECENTLY_FINISHED)},
      static_cast<uint8_t>(viewMode));
  if (!selector) {
    LOG_ERR("RBA", "Could not allocate library view selector");
    return;
  }
  startActivityForResult(std::move(selector), [this](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto* selected = std::get_if<OptionSelectionResult>(&result.data);
    if (!selected || selected->index > static_cast<uint8_t>(ViewMode::RecentlyFinished)) return;
    viewMode = static_cast<ViewMode>(selected->index);
    selectorIndex = 0;
    topIndex = 0;
    reloadAfterBookAction();
  });
}

void RecentBooksActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<RecentBooksActivity*>(user);
  if (event.value == 0) {
    self->app.clearTapFlash();
    self->showViewSelector();
    return;
  }
  if (event.value < 1 || event.value > static_cast<int16_t>(self->recentBooks.size())) return;
  self->selectorIndex = static_cast<size_t>(event.value);
  if (event.longPress) {
    self->app.clearTapFlash();
    self->showBookActionMenu(self->selectorIndex - 1);
    return;
  }
  // Opening the book leaves this screen; a lingering flash would gray an
  // unrelated row when the list next appears.
  self->app.clearTapFlash();
  self->onSelectBook(self->recentBooks[self->selectorIndex - 1].path);
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Prune entries whose backing files are gone; this is one of two interaction
  // points where the persistent store gets cleaned.
  if (viewMode == ViewMode::RecentlyOpened && RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  // Load data
  loadRecentBooks();

  selectorIndex = 0;
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &RecentBooksActivity::onRowEvent, this);
  app.setScreen(&RecentBooksActivity::listScreen, this);
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  std::vector<RecentBook>().swap(recentBooks);
}

void RecentBooksActivity::loop() {
  if (pendingCacheDeletedFeedback && millis() - cacheDeletedFeedbackShowTime >= ACTION_FEEDBACK_MS) {
    pendingCacheDeletedFeedback = false;
    requestUpdate();
    return;
  }

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    onGoHome();
    return;
  }
  const int listSize = static_cast<int>(recentBooks.size()) + 1;
  // After a long-press has fired, swallow input until Confirm is physically released
  // (so the release doesn't also open the book; re-arm only once the button is up).
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
    }
    return;
  }

  // Long-press Confirm on the selected book: open the same action menu shape used by File Browser.
  // Fires when the hold times out while still held (firmware hold-to-act pattern,
  // cf. FileBrowserActivity BACK long-press).
  if (selectorIndex > 0 && selectorIndex <= recentBooks.size() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired = true;
    showBookActionMenu(selectorIndex - 1, true);
    return;
  }

  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent dispatch.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      // No pressed-state repaint: the render it triggers would drop a slow
      // tap's release inside the uiReady window (tap-to-activate needed two
      // taps), and it costs a second e-ink refresh per tap.
      if (app.invalidated()) requestUpdate();
      if (event) return;  // dispatched to onRowEvent
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex == 0) {
      showViewSelector();
      return;
    }
    if (selectorIndex <= recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex - 1].path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  // Swipes scroll the viewport; the selection stays put and button navigation
  // pulls the view back to it.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
    const int next = scrollListBy(topIndex, delta, visibleRows, listSize);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }

  const auto moveSelection = [this, listSize](const int index) {
    selectorIndex = static_cast<size_t>(index);
    topIndex = followListSelection(static_cast<int>(selectorIndex), topIndex, visibleRows, listSize);
    requestUpdate();
  };
  buttonNavigator.onNextRelease([this, listSize, &moveSelection] {
    moveSelection(ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize));
  });
  buttonNavigator.onPreviousRelease([this, listSize, &moveSelection] {
    moveSelection(ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize));
  });
  buttonNavigator.onNextContinuous([this, listSize, &moveSelection] {
    moveSelection(ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, visibleRows));
  });
  buttonNavigator.onPreviousContinuous([this, listSize, &moveSelection] {
    moveSelection(ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, visibleRows));
  });
}

void RecentBooksActivity::reloadAfterBookAction() {
  loadRecentBooks();
  if (recentBooks.empty()) {
    selectorIndex = 0;
  } else if (selectorIndex > recentBooks.size()) {
    selectorIndex = recentBooks.size();
  }
  topIndex = followListSelection(static_cast<int>(selectorIndex), topIndex, visibleRows,
                                 static_cast<int>(recentBooks.size()) + 1);
  requestUpdate(true);
}

void RecentBooksActivity::promptDeleteBook(const RecentBook& book) {
  const std::string path = book.path;
  const bool inTrash = crosshatch::trash::isPath(path.c_str());
  const bool moveToTrash = SETTINGS.recycleBinEnabled && !inTrash;
  auto handler = [this, path, moveToTrash](const ActivityResult& res) {
    if (res.isCancelled) {
      return;
    }

    bool moved = false;
    if (!BookActions::deleteOrTrashFile(path, moved)) {
      LOG_ERR("RBA", "Failed to delete/trash file: %s", path.c_str());
      return;
    }

    if (moved) {
      BookActions::drawToast(renderer, tr(STR_MOVED_TO_TRASH));
      delay(1000);
    }

    reloadAfterBookAction();
  };

  const StrId labelId =
      inTrash ? StrId::STR_PERMANENT_DELETE : (moveToTrash ? StrId::STR_MOVE_TO_TRASH : StrId::STR_DELETE);
  const std::string heading = BookActions::confirmationHeading(labelId);
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, book.title),
                         std::move(handler));
}

void RecentBooksActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      reloadAfterBookAction();
    }
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title,
                                             /*ignoreInitialConfirmRelease=*/false),
      std::move(handler));
}

void RecentBooksActivity::showBookActionMenu(const size_t bookIndex, const bool ignoreInitialConfirmRelease) {
  if (bookIndex >= recentBooks.size()) return;

  const RecentBook book = recentBooks[bookIndex];
  std::vector<FileBrowserActionActivity::MenuItem> items =
      BookActions::buildBookActionItems(book.path, viewMode == ViewMode::RecentlyOpened);
  if (BookActions::canSendNearby(book.path)) {
    items.push_back({FileBrowserAction::SendNearby, StrId::STR_SEND_NEARBY_BOOK});
  }

  startActivityForResult(
      std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, book.title, std::move(items),
                                                  ignoreInitialConfirmRelease),
      [this, book](const ActivityResult& result) {
        longPressFired = false;
        if (result.isCancelled) {
          return;
        }

        const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
        if (!actionResult) {
          LOG_ERR("RBA", "Book action result missing");
          return;
        }

        const auto action = static_cast<FileBrowserAction>(actionResult->action);
        StrId feedback = StrId::STR_LIBRARY_SAVE_FAILED;
        if (BookActions::handleLibraryAction(action, book.path, feedback)) {
          BookActions::drawToast(renderer, I18N.get(feedback));
          delay(ACTION_FEEDBACK_MS);
          reloadAfterBookAction();
          return;
        }
        switch (action) {
          case FileBrowserAction::Delete:
            promptDeleteBook(book);
            return;
          case FileBrowserAction::DeleteCache:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(
                    renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE_CACHE), book.title),
                [this, book](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled) {
                    if (!BookActions::clearBookCache(book.path)) {
                      LOG_ERR("RBA", "Failed to clear book cache for: %s", book.path.c_str());
                    } else {
                      pendingCacheDeletedFeedback = true;
                      cacheDeletedFeedbackShowTime = millis();
                    }
                  }
                  reloadAfterBookAction();
                });
            return;
          case FileBrowserAction::DeleteStats:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(
                    renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE_BOOK_STATS), book.title),
                [this, book](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled) {
                    if (!BookActions::deleteBookStats(book.path)) {
                      LOG_ERR("RBA", "Failed to delete book stats for: %s", book.path.c_str());
                    } else {
                      BookActions::drawToast(renderer, tr(STR_BOOK_STATS_DELETED));
                      delay(1000);
                    }
                  }
                  reloadAfterBookAction();
                });
            return;
          case FileBrowserAction::ResetReaderSettings:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(
                    renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_RESET_BOOK_READER_SETTINGS),
                    book.title),
                [this, book](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled) {
                    if (!BookActions::resetBookReaderSettings(book.path)) {
                      LOG_ERR("RBA", "Failed to reset reader settings for: %s", book.path.c_str());
                    } else {
                      BookActions::drawToast(renderer, tr(STR_BOOK_READER_SETTINGS_RESET));
                      delay(1000);
                    }
                  }
                  reloadAfterBookAction();
                });
            return;
          case FileBrowserAction::ToggleCompleted: {
            bool completed = false;
            if (BookActions::toggleBookCompleted(book.path, book.title, completed)) {
              BookActions::drawToast(renderer, completed ? tr(STR_MARKED_FINISHED) : tr(STR_MARKED_UNFINISHED));
              delay(1000);
            }
            reloadAfterBookAction();
            return;
          }
          case FileBrowserAction::EpubRenderMode: {
            const uint8_t currentIndex =
                BookActions::epubRenderModeDisplayIndex(EpubReaderActivity::loadBookRenderMode(book.path));
            startActivityForResult(
                std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "RecentEpubRenderModeSelect",
                                                          StrId::STR_EPUB_RENDER_MODE,
                                                          BookActions::epubRenderModeOptions(), currentIndex),
                [this, book](const ActivityResult& selectionResult) {
                  if (!selectionResult.isCancelled) {
                    const auto* selection = std::get_if<OptionSelectionResult>(&selectionResult.data);
                    if (selection != nullptr &&
                        !EpubReaderActivity::saveBookRenderMode(
                            book.path, BookActions::epubRenderModeForDisplayIndex(selection->index))) {
                      LOG_ERR("RBA", "Failed to save render mode for: %s", book.path.c_str());
                    }
                  }
                  reloadAfterBookAction();
                });
            return;
          }
          case FileBrowserAction::TogglePinnedToHome: {
            bool pinned = false;
            if (BookActions::togglePinnedToHome(book.path, pinned)) {
              BookActions::drawToast(renderer, pinned ? tr(STR_PINNED_TO_HOME) : tr(STR_UNPINNED_FROM_HOME));
              delay(1000);
            }
            requestUpdate();
            return;
          }
          case FileBrowserAction::RemoveFromRecents:
            promptRemoveBook(book.path, book.title);
            return;
          case FileBrowserAction::SendNearby:
            activityManager.goToNearbyBookSend(book.path, false);
            return;
          case FileBrowserAction::LibraryView:
          case FileBrowserAction::AssignDocA:
          case FileBrowserAction::AssignDocB:
          case FileBrowserAction::PinDocument:
          case FileBrowserAction::UnpinDocument:
          case FileBrowserAction::PinFolder:
          case FileBrowserAction::UnpinFolder:
          case FileBrowserAction::SwitchDoc:
          case FileBrowserAction::Restore:
          case FileBrowserAction::EmptyTrash:
          case FileBrowserAction::PinFavorite:
          case FileBrowserAction::UnpinFavorite:
          case FileBrowserAction::PinBootFavorite:
          case FileBrowserAction::UnpinBootFavorite:
          case FileBrowserAction::SetSleepFolder:
          case FileBrowserAction::ClearSleepFolder:
          case FileBrowserAction::ViewBookmarks:
          case FileBrowserAction::ViewClippings:
          case FileBrowserAction::DeleteBookmarks:
          case FileBrowserAction::DeleteClippings:
            return;
        }
      });
}

void RecentBooksActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<RecentBooksActivity*>(user)->buildListScreen(screen);
}

void RecentBooksActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (viewMode != ViewMode::RecentlyOpened) {
    const auto style = screen.theme().smallText;
    const int16_t lineHeight = uiTarget.lineHeight(style.font);
    if (scanStatus.failed || scanStatus.limited) {
      uiTarget.text(screen.takeBottom(lineHeight),
                    scanStatus.failed ? tr(STR_VIRTUAL_SCAN_FAILED) : tr(STR_VIRTUAL_SCAN_LIMIT), style);
    }
    if (viewMode == ViewMode::RecentlyAdded) {
      uiTarget.text(screen.takeBottom(lineHeight), tr(STR_VIRTUAL_MODIFIED_ORDER), style);
    } else if (scanStatus.unknownDates) {
      uiTarget.text(screen.takeBottom(lineHeight), tr(STR_VIRTUAL_UNKNOWN_DATES), style);
    }
    uiTarget.text(screen.takeBottom(lineHeight), tr(STR_VIRTUAL_SCAN_SCOPE), style);
  }

  listItems[0] = {};
  listItems[0].label = tr(STR_LIBRARY_VIEW);
  listItems[0].subtitle = viewTitle();
  listItems[0].actionValue = 0;
  size_t itemCount = 1;
  for (const auto& book : recentBooks) {
    auto& item = listItems[itemCount];
    item = {};
    item.label = book.title.c_str();
    if (!book.author.empty()) item.subtitle = book.author.c_str();
    item.icon = listIconFor(UITheme::getFileIcon(book.path), 32);
    item.actionValue = static_cast<int16_t>(itemCount++);
  }

  fui::ListProps props;
  props.items = listItems.data();
  props.count = static_cast<uint16_t>(itemCount);
  props.selectedIndex = static_cast<int16_t>(selectorIndex);
  props.action = ACTION_ROW;
  props.inputMask = static_cast<uint16_t>(fui::InputTouch | fui::InputLongPress);  // physical buttons stay in loop()
  props.iconSize = 28;
  props.labelText = screen.theme().bodyText;
  props.labelText.bold = true;
  const fui::Rect listBounds = screen.body();
  const auto rows = configureUiList(props, screen.theme(), listBounds, UiListRowType::WithSubtitle);
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(itemCount));  // clamp to range
  props.topIndex = static_cast<uint16_t>(topIndex);
  if (recentBooks.empty()) {
    screen.list(props, static_cast<int16_t>(props.rowHeight + props.rowGap));
    screen.centeredText(viewMode == ViewMode::RecentlyFinished ? tr(STR_NO_FINISHED_BOOKS)
                        : viewMode == ViewMode::RecentlyAdded  ? tr(STR_NO_RECENTLY_ADDED_BOOKS)
                                                               : tr(STR_NO_RECENT_BOOKS),
                        screen.theme().bodyText);
  } else {
    screen.list(props);
  }
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, viewTitle(), false);
  } else {
    GUI.drawHeader(renderer, header, viewTitle());
  }

  uiReady = false;
  app.render();
  uiReady = true;

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_HOME)), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (pendingCacheDeletedFeedback) {
    GUI.drawPopup(renderer, tr(STR_BOOK_CACHE_DELETED));
  }

  renderer.displayBuffer();
}
