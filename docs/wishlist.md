# CrossHatch — Personal Wishlist

Recorded: 2026-09-21

CrossHatch is a personal-only CrossInk fork focused on the Xteink X4 Pro.
This document records interests and preservation requirements, not a scheduled
roadmap or authorization to implement them. Priorities, effort estimates, and
implementation details remain open. No new feature is claimed implemented or
validated by its inclusion here.

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
progress and positions take precedence; highlights and clippings are considered
lower-priority during early phases. Reader improvements should not silently reset
or discard reading state.

### F4. Current custom sleep image

Keep the current custom sleep-image option and the user's selected-image
workflow fully available as a selectable option, rather than replacing it.
Dynamic sleep cards (W3) must be an optional alternative mode, never an automatic
override or side-effect overwrite of the custom image.

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

### W3. Optional dynamic sleep cards

Consider useful personal cards such as a short task list, rehearsal priorities,
maintenance reminders, or a music-practice constraint. Mac-generated snapshots
and locally composed cards are possibilities, not selected implementations.
Show preparation/update information where freshness matters. Keep a usable
static fallback and do not require an always-on connection or periodic wakeups.
Never overwrite the user's selected custom image as a side effect.

### W4. Better web transfer and selected-source integrations

Make loading material onto CrossHatch simpler and more reliable, including
clear destination selection and transfer results. Prioritize a supported path
from Drafts. Consider Obsidian integration through a plugin or contributor-
supplied adapter, without making Obsidian a required dependency.

Prefer a small set of documented, supported sources over a general integration
framework. Keep generic file transfer available. Transfer protocols, document
identity, revision handling, and any synchronization behavior remain open.
A supported source is not permission for unrestricted network access.

### W5. Offline checklist runner

Consider reusable personal checklists for preshow checks, maintenance, equipment
setup, and development testing. Support clear progress, locally saved completion,
and deliberate reset/reuse. It should work offline and should not create a new
showtime online-recordkeeping obligation for crew.

### W6. A/B hopping

Consider two explicitly retained document/location slots for quick switching
between primary reading and a reference. Preserve each position and make the
active slot clear. This does not require keeping two reader engines resident.

### W7. Print to CrossHatch

Consider a Mac-side print/PDF preparation and delivery workflow that makes
material readable on the X4 Pro rather than simply shrinking full-size pages.
Cropping, splitting, and source-page references are possibilities. Output
formats and interfaces remain open; native on-device PDF rendering is not a
prerequisite or an approved implementation direction.

### W8. Read-only Booth Status Companion

Consider a small foreground viewer for selected booth status snapshots.
Start from explicit manual requests, or carefully bounded refresh behavior
while the activity is open. Show the last successful update and clearly
identify cached, stale, or disconnected data; do not present a retained e-ink
snapshot as live status.

Keep it read-only: no cue firing, equipment control, or dependence on it for
time-critical show operation. Data sources, connection/security design, and
refresh cadence remain open. Closing the activity should end its refresh work.

## Initial Implementation Plan

Difficulty assessment and phased roadmap based on technical risk, architectural
fit, and development ROI.

### Difficulty Ranking

| Rank | ID | Title | Difficulty (1–10) | Scope & Technical Rationale |
|:---:|:---:|:---|:---:|:---|
| **1** | **W1** | Remove or hide reading statistics | **1 / 10** | Settings/UI toggles; `trackReadingStats` already exists in settings; near-zero regression risk. |
| **2** | **W6** | A/B hopping | **3 / 10** | Reader flow; dual-slot position tracker via `APP_STATE` and Quick Actions; no dual-engine overhead. |
| **3** | **W2** | Configurable Quick Action pins | **4 / 10** | Extending settings persistence for pinned paths/tools and settings picker UI. |
| **4** | **W5** | Offline checklist runner | **5 / 10** | Standalone foreground Activity, SD file I/O, touch/button list navigation, offline state persistence. |
| **5** | **W3** | Optional dynamic sleep cards | **5.5 / 10** | Sleep Activity layout addition; renders offline text/reminders without overriding F4. |
| **6** | **W8** | Booth Status Companion | **6.5 / 10** | Wi-Fi lifecycle, JSON parsing, clear stale/disconnected e-ink state, clean exit task cleanup. |
| **7** | **W4** | Web transfer & app sources | **7.5 / 10** | Embedded web server endpoints, upload handling, web portal UI, memory discipline on uploads. |
| **8** | **W7** | Print to CrossHatch | **8 / 10** | Cross-platform pipeline (Mac-side PDF split/dither/format conversion + device ingestion). |

### Phased Roadmap

#### Phase 1: Environment Baseline & Immediate Win
- **F1 — Streamline X4 Pro Profile & Simulator**: Verify and streamline `x4-pro` and `x4-pro-simulator` builds as the primary personal development baseline.
- **W1 — Remove / Hide Reading Stats UI**: Early easy win; hide/remove statistics UI menus, frontlight shortcuts, and sleep stats screens.

#### Phase 2: Core Device & Reader Quality of Life
- **W6 — A/B Document Hopping**: Fast switching between primary reading and reference materials.
- **W2 — Configurable Quick Actions & Pins**: Pin frequently accessed folders/documents (e.g. Inbox, Current Show) to shortcuts.
- **W3 — Optional Dynamic Sleep Cards**: Add support for dynamic task/rehearsal cards as an optional sleep screen mode while preserving F4.

#### Phase 3: Foreground Tools (Adhering to F2)
- **W5 — Offline Checklist Runner**: Self-contained checklist tool for prep, preshow checks, and equipment maintenance.
- **W8 — Read-only Booth Status Companion**: Foreground-only read-only viewer for snapshot updates with explicit stale-state handling.

#### Phase 4: Integrations & External Pipelines
- **W4 — Better Web Transfer & Direct App Endpoints**: Endpoints for single-tap drops from Drafts and Obsidian.
- **W7 — Print to CrossHatch**: External Mac preparation toolchain + device ingest for legible page delivery.

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
