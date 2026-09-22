# CrossHatch — Personal Wishlist

Recorded: 2026-09-21

CrossHatch is a personal-only CrossInk fork focused on the Xteink X4 Pro.
This document records interests and preservation requirements, not a scheduled
roadmap or authorization to implement them. Priorities, effort estimates, and
implementation details remain open. No new feature is claimed implemented or
validated by its inclusion here.

## General Fork-Reuse Policy

CrossHatch is explicitly willing to borrow or adapt working implementations from
other CrossPoint/CrossInk-derived firmware when they fit CrossHatch's architecture
and goals. Prefer proven code over reimplementing a feature solely for originality.

Before borrowing code:
1. **Inspect the source implementation** directly rather than relying on its README description.
2. **Verify license compatibility** and preserve any required attribution or notices.
3. **Adapt to CrossHatch** rather than importing unrelated architecture, conventions, or UX paradigms.
4. **Preserve CrossInk baseline behavior** where the borrowed feature is optional.
5. **Prefer X4 Pro-specific capability gating** where that substantially simplifies or improves an implementation.

Useful reference forks include:
- **Tesserae CrossInk**: [https://github.com/dmellok/CrossInk](https://github.com/dmellok/CrossInk) (e-ink client integration)
- **Tesserae Server**: [https://github.com/dmellok/tesserae](https://github.com/dmellok/tesserae) (server-side rendering and cards)
- **Papyrix**: [https://github.com/bigbag/papyrix-reader](https://github.com/bigbag/papyrix-reader) (Recycle bin / trash, AirPrint/IPP implementation)
- **CrumBLE**: [https://github.com/imshentastic/CrumBLE](https://github.com/imshentastic/CrumBLE) (Smart collections, BLE exploration)
- **XPoint**: [https://github.com/Belphemur/XPoint](https://github.com/Belphemur/XPoint) (Hardware variants and features)
- **CrossPointXT**: [https://github.com/lukecarbis/CrossPointXT](https://github.com/lukecarbis/CrossPointXT) (Reader and UI enhancements)

*Note: These are references for prior art and code reuse, not runtime dependencies by default.*

## Existing foundations and personal behavior to preserve

### F1. X4 Pro-specific build and simulator

Use the existing `x4-pro` firmware and `x4-pro-simulator` profiles as the focus
for personal development and evaluation. Prioritize streamlining these environments
early so development and testing iterations are fast and repeatable. This does
not authorize removing other device profiles or bypassing shared-code compatibility
requirements. Simulator checks and testing on the actual X4 Pro remain separate.

### F2. Foreground Activity System

Keep and extend the existing foreground Activity system. New tools should have
bounded lifecycles, release their resources when closed, and return cleanly to
reading. This is not a proposal for a new plugin platform or background-service
framework.

### F3. Bookmarks, highlights, clippings, and per-book settings

Preserve reading features, saved content, and reading positions. Essential reading
progress, bookmarks, and per-book settings take precedence; highlights and clippings
are considered deferred / low priority during early phases and are not compelling
enough to drive development. Reader improvements must not silently reset or discard
reading state.

### F4. Current custom sleep image

Keep the current custom sleep-image option and the user's selected-image
workflow fully available as a selectable option, rather than replacing it.
Dynamic sleep cards (W3) must be an optional alternative mode, never an automatic
override or side-effect overwrite of the custom image.

### F5. Recycle Bin / Trash

Add recoverable document deletion so deleting a user document is recoverable
rather than immediately destructive.
- Deleting normally moves the item to a dedicated trash/recycle-bin directory on SD.
- Provide a clear way to restore an item to its original or default location.
- Provide explicit permanent-delete and empty-trash actions.
- Avoid unnecessary complexity such as version history or tiered retention policies.
- Account for metadata/cache cleanup (EPUB cache, bookmarks, reading progress)
  when an item is permanently removed.
- **Reference**: Investigate Papyrix's existing implementation first and reuse/adapt
  it if practical.

### F6. Recently-* virtual views

CrossHatch does not currently need a full generalized collections/smart-folder
system, but several automatic library views are desirable:
- **Recently Added**
- **Recently Opened / Recently Read**
- **Recently Finished**

These views should preferably be generated dynamically from existing metadata
(`RecentBooksStore`, file timestamps, reading stats/history if tracked) rather
than physically reorganizing files on the SD card.
- **Reference**: Investigate CrumBLE's smart collections for reusable implementation
  ideas, without committing CrossHatch to CrumBLE's complete collection-management system.

## Modifications to consider

### W1. Remove or hide reading statistics

Provide a reading experience without unwanted statistics UI. Whether to hide
the UI, disable collection, or remove parts of the subsystem remains open.
Keep this separate from essential reading progress and saved reader data.
(Easy win, should be completed early).

### W2. Configurable Quick Actions, including pinned destinations

Preserve and extend configurable Quick Actions. Consider pins for specific
folders, documents, and foreground tools, such as Inbox, Reference, or Current
Show. Exact menu layout, number of pins, and configuration method are open;
reuse the existing action system where practical.

### W3. Optional dynamic sleep cards (Tesserae-style)

Deferred. The following concept is retained for future reconsideration, not
active implementation scope:
- Retain CrossInk/CrossHatch's current local custom sleep-image feature unchanged (F4);
  dynamic cards remain optional.
- Allow a configured local or self-hosted server (Mac, Linux machine, Raspberry Pi,
  Docker host, etc.) to provide a display-ready 1-bit frame. CrossHatch does not
  require a cloud service.
- The external server performs expensive HTML parsing, external data fetches, and
  layout rendering rather than the X4 Pro.
- Cache the last known-good dynamic frame locally on SD storage.
- Network or server failure must never prevent sleep or normal device operation;
  fall back cleanly to the cached frame or the configured static/custom sleep image.
- Avoid requiring continuous or periodic background Wi-Fi connectivity.
- Potential card targets include CrossHatch-local information, useful personal
  dashboards, rehearsal notes, maintenance reminders, and read-only Booth Status.
- **Reference**: Inspect Tesserae CrossInk client integration (`dmellok/CrossInk`)
  and determine how much can be borrowed directly, explicitly validating the path
  on the X4 Pro.

### W4. Better web transfer and selected-source integrations

Scope pending reconsideration. Start from a concrete transfer workflow before
selecting integrations or protocols. Earlier ideas below are candidates only.

Make loading material onto CrossHatch simpler and more reliable, including
clear destination selection and transfer results. Prioritize a supported path
from Drafts. Consider Obsidian integration through a plugin or contributor-
supplied adapter, without making Obsidian a required dependency.

Prefer a small set of documented, supported sources over a general integration
framework. Keep generic file transfer available. Transfer protocols, document
identity, revision handling, and any synchronization behavior remain open.
A supported source is not permission for unrestricted network access. A unified
import pipeline (W7) is not a prerequisite or current commitment.

### W5. Offline Markdown Checklist Viewer

Use ordinary `.md` files with Markdown task-list items (`- [ ]` unchecked,
`- [x]` checked), so checklists can be created in a text editor and transferred
using existing file-transfer paths. Display headings and task text for reusable
personal checklists covering preshow checks, maintenance, equipment setup, and
development testing. Support clear progress, checking items, locally saved
completion, and deliberate reset/reuse. Completion updates the source Markdown
so checked items remain portable when copied off the device. It should work
offline and should not create a new showtime online-recordkeeping obligation
for crew.

### W6. A/B hopping

Consider two explicitly retained document/location slots for quick switching
between primary reading and a reference. Preserve each position and make the
active slot clear. This does not require keeping two reader engines resident.

### W7. Print to CrossHatch / unified import pipeline (including AirPrint & Bluetooth)

Deferred while transfer needs are reconsidered. The earlier proposal below is
background only; it does not define the next implementation scope.

Treat "Print to CrossHatch" conceptually as a **unified document import pipeline**,
not as a single transport. Multiple transports should eventually feed the same
receiving/import destination (e.g. a predictable `/Inbox` or import folder on SD),
after which the item behaves like an ordinary CrossHatch document:
- Existing CrossInk web transfer.
- Trusted/blessed Drafts workflows.
- Possible Obsidian tooling.
- Mac-side print/PDF preparation: preparation and delivery workflow that formats
  material for legibility on the X4 Pro (cropping margins, splitting pages,
  dithering to 1-bit, generating image sets or CBZ/EPUB/XTC/TXT) rather than shrinking
  full-size pages. Native on-device PDF rendering is not a prerequisite.
- **AirPrint / Network Printing**: Investigate Papyrix's existing AirPrint/IPP
  implementation to allow CrossHatch to appear as a print target from Apple devices
  when deliberately placed in a network receive mode (no continuous background Wi-Fi).
- **Bluetooth Transfer (BLE Receive)**: Investigate an offline-friendly BLE transfer
  protocol feeding the same Inbox pipeline when Wi-Fi is undesirable.
  - Sender packages and sends a document over BLE; CrossHatch stores it via the
    normal import pipeline.
  - Optional metadata for open-after-transfer, target path, or replacement of named items.
  - Initial investigation must evaluate realistic throughput on ESP32-S3, BLE
    coexistence while CrossHatch runs, file-size limits, resume/error handling, and
    sender tooling (e.g. small `crosshatch-send` Mac CLI, Share extension, or Shortcut).
- Avoid designing separate, isolated document-management systems for each transport.

### W8. Read-only Booth Status Companion

Deferred; excluded from the current Phase 3 scope.

Consider a small foreground viewer for selected booth status snapshots.
Start from explicit manual requests, or carefully bounded refresh behavior
while the activity is open. Show the last successful update and clearly
identify cached, stale, or disconnected data; do not present a retained e-ink
snapshot as live status.

Keep it read-only: no cue firing, equipment control, or dependence on it for
time-critical show operation. Data sources, connection/security design, and
refresh cadence remain open. Closing the activity should end its refresh work.
(Can potentially share card formatting with W3).

### W9. General smart folders / collections

Keep generalized smart folders or user-defined collections as optional,
lower-priority exploration. They are less important than the automatic Recently-*
views (F6).
- If eventually implemented, collections should ideally be metadata views rather
  than moving or duplicating source files on storage.
- Ordinary SD filesystem folder organization must continue to work transparently.
- Avoid turning library management into an oversized database subsystem.
- **Reference**: CrumBLE is the primary reference implementation to inspect.

## Project Non-Goals & Deferred Items

- **Highlights & Clippings**: Low priority; retained as deferred baseline features,
  not actively expanded or prioritized based on forks like CrumBLE.
- **Flashcards / Anki / Spaced Repetition**: Explicit non-goal. While projects like
  CrossPet or CrossPlay demonstrate alternate activity types, spaced repetition is
  outside CrossHatch's product direction.
- **Subscription / Read-Later Services**: Non-goal. Cloud subscription services
  (Instapaper, Pocket, etc.) are excluded. CrossHatch favors local, self-hosted,
  open-protocol, or user-controlled transfer paths.

## Initial Implementation Plan

Difficulty assessment and phased roadmap based on technical risk, architectural
fit, and development ROI.

### Difficulty Ranking

| Rank | ID | Title | Difficulty (1–10) | Scope & Technical Rationale |
|:---:|:---:|:---|:---:|:---|
| **1** | **W1** | Remove or hide reading statistics | **1 / 10** | Settings/UI toggles; `trackReadingStats` already exists in settings; near-zero regression risk. |
| **2** | **F5** | Recycle Bin / Trash | **2.5 / 10** | File operations in `FsHelpers` / `BookActions`; move to `.trash/`, restore, empty trash. Prior art in Papyrix. |
| **3** | **W6** | A/B hopping | **3 / 10** | Reader flow; dual-slot position tracker via `APP_STATE` and Quick Actions; no dual-engine overhead. |
| **4** | **F6** | Recently-* virtual views | **3.5 / 10** | Dynamic view over `RecentBooksStore` and SD file timestamps; no file reorganizing. Prior art in CrumBLE. |
| **5** | **W2** | Configurable Quick Action pins | **4 / 10** | Extending settings persistence for pinned paths/tools and settings picker UI. |
| **6** | **W5** | Offline Markdown Checklist Viewer | **5 / 10** | Standalone foreground Activity, Markdown task lists on SD, touch/button list navigation, offline state persistence. |
| **7** | **W3** | Optional dynamic sleep cards (deferred) | **5.5 / 10** | Sleep Activity layout addition; local frame caching; server rendering offloaded to Tesserae. Preserves F4. |
| **8** | **W8** | Booth Status Companion | **6.5 / 10** | Wi-Fi lifecycle, JSON parsing, clear stale/disconnected e-ink state, clean exit task cleanup. |
| **9** | **W4** | Web transfer & app sources (scope pending) | **7.5 / 10** | Embedded web server endpoints, upload handling, web portal UI, memory discipline on uploads. |
| **10**| **W7** | Print to CrossHatch & unified import (deferred) | **8 / 10** | Unified Inbox pipeline across Mac PDF tooling, AirPrint/IPP (Papyrix), and BLE receive transport. |
| **11**| **W9** | General smart folders / collections | **8.5 / 10**| Low-priority complex metadata collection system (CrumBLE reference). |

Difficulty estimates above describe the earlier concepts; they do not set priority
or estimate the smaller transfer scope still to be defined.

### Phased Roadmap

#### Phase 1: Environment Baseline & Immediate Wins (Core Hygiene)
- **F1 — Streamline X4 Pro Profile & Simulator**: Verify and streamline `x4-pro` and `x4-pro-simulator` builds as the primary personal development baseline.
- **W1 — Remove / Hide Reading Stats UI**: Early easy win; hide/remove statistics UI menus, frontlight shortcuts, and sleep stats screens.
- **F5 — Recycle Bin / Trash**: Essential safeguard before building heavy import or navigation workflows. Move deleted files to `.trash/` with restore and empty-trash actions, adapting Papyrix's proven model.

#### Phase 2: Offline Reader & Library Ergonomics (High Applicability, Zero Network)
*Focus: Maximize daily reading efficiency completely offline with low-to-moderate effort.*
- **W6 — A/B Document Hopping**: Implemented with two explicitly assigned paths, reader menus, and Quick Actions/Power/Home shortcuts. Switching flushes pending progress and destroys the previous reader before loading the destination; an unavailable target or failed progress save keeps the current reader open. Opening another book does not overwrite either assignment. Switching time requires hardware measurement.
- **F6 — Recently-* Virtual Views**: Implemented with an explicit Library view selector that preserves page navigation. Added/Finished scan up to 18 results in root and two folder levels, stopping at 2,048 entries or a two-second scan budget with incomplete-scan feedback. Added sorts by modification date, which may differ from the date copied onto SD. Finished uses explicit EPUB/XTC completion or an existing EPUB progress cache of at least 99.5%, independently of recents. Unknown dates sort last; the current simulator HAL has no file-date API. Hidden folders and `/trash` are excluded; no files are reorganized.
- **W2 — Configurable Quick Actions & Pins**: Implemented with one document and one folder pin, plus shortcuts to all three views. Pinning fills the first empty popup slot; a full popup keeps its existing assignments. Configure additional slots/Power/Home bindings in Settings. Missing or moved targets retain their assigned paths and report unavailable.

Validation: native tests plus `python3 scripts/run_simulator_smoke_test.py --env x4-pro-simulator --library` exercise saved EPUB/TXT positions, missing targets, pins, and finished history. On X4 Pro and X3/X4 hardware, verify each book resumes at its previous page after repeated A/B hops, reopen after sleep, check dated SD files in Added, and confirm a finished book remains discoverable after removal from recents. No cache reset is required; cache formats are unchanged. Physical timing, SD failure behavior, and long-running heap stability remain hardware checks.

#### Phase 3: Bounded Foreground Utility Activities (F2 Adherence)
*Focus: Expand device utility using isolated `Activity` lifecycles that guarantee clean memory teardown.*
- **W5 — Offline Markdown Checklist Viewer**: Implemented for ordinary `.md` task lists opened from Library, with headings, touch/button checking, completion saved directly into the Markdown file, and confirmed reset. Ordinary Markdown retains its text-reader fallback; the viewer also offers **View Markdown text**. Limits are 64 KiB per file, 128 headings/items, 8 KiB of labels, and 255 bytes per heading/item line. Saves preserve other source bytes and refuse externally changed files; temporary/backup files support recovery. Booth status remains deferred.

Validation: parser/save-failure native tests and `python3 scripts/run_simulator_smoke_test.py --env x4-pro-simulator --checklist` cover Library entry, toggling, reopening, reset cancellation/confirmation, touch, long-list navigation, and plain-Markdown fallback. Repeat with `--env simulator` for button controls. Physical SD errors, power-loss recovery, sleep/wake, and memory stability remain hardware checks; see [Markdown checklists](reader-features.md#markdown-checklists). No cache format change or reset is required.

#### Phase 4: Dynamic Sleep Cards — Deferred
- **W3 — Optional Dynamic Sleep Cards**: Deferred for future reconsideration.

#### Phase 5: Transfers — Scope to Be Rethought
- **W4 — Transfer workflow**: Revisit the actual source, destination, and manual steps that need improvement before choosing a small implementation scope.
- **W7 — Expanded import pipeline**: Deferred. Unified Inbox architecture, Mac PDF tooling, AirPrint/IPP, BLE, and dedicated app integrations are not committed deliverables.
- No replacement transfer design or implementation is selected yet.

#### Deferred / Low Priority
- **W8 — Read-only Booth Status Companion**: Deferred for now; data sources, connection/security design, and refresh cadence remain open.
- **W9 — General Smart Folders / Collections**: User-defined tag/collection subsystem (complex metadata tracking; deferred behind automatic F6 views).
- **Highlights & Clippings**: Deferred baseline features; preserved (F3) but not actively driven.

## Scope

These are the selected wishlist items for now. Other ideas from earlier
brainstorming are not implicitly included, rejected forever, or implementation
commitments.

## Repository reference points

Existing architecture and feature references, not a build or hardware test report:

- `AGENTS.md` and `platformio.ini`
- `src/activities/ActivityManager.h`
- `src/QuickActions.h` and `src/QuickActions.cpp`
- `docs/controls.md` and `docs/reader-features.md`
- `docs/data-cache.md` and `docs/webserver.md`
