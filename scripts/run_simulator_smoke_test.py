#!/usr/bin/env python3
"""Build and run the simulator smoke test against an isolated fs_ directory."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BOOK = ROOT / "test" / "epubs" / "test_reader_rendering_matrix.epub"
CRASH_PATTERNS = (
    "std::bad_alloc",
    "terminating due to uncaught exception",
    "Assertion failed",
    "Segmentation fault",
    "AddressSanitizer",
    "UndefinedBehaviorSanitizer",
)
THEMES = {
    "classic": 0,
    "lyra": 1,
    "lyra-extended": 2,
    "lyra_extended": 2,
    "lyra3": 2,
    "lyra-3-covers": 2,
    "roundedraff": 3,
    "rounded-raff": 3,
    "lyra-carousel": 4,
    "lyra_carousel": 4,
    "carousel": 4,
    "dashboard": 6,
}


def program_path(env_name: str) -> Path:
    return ROOT / ".pio" / "build" / env_name / "program"


def build_simulator(env_name: str) -> None:
    print(f"Building {env_name} simulator...", flush=True)
    proc = subprocess.run(["pio", "run", "-e", env_name], cwd=ROOT)
    if proc.returncode != 0:
        raise SystemExit(proc.returncode)


def prepare_fs(temp_root: Path, book: Path) -> str:
    books_dir = temp_root / "fs_" / "books"
    books_dir.mkdir(parents=True, exist_ok=True)

    target = books_dir / book.name
    shutil.copy2(book, target)
    return f"/books/{book.name}"


def run_smoke(args: argparse.Namespace) -> int:
    book = Path(args.book).resolve()
    if not book.exists():
        print(f"Smoke test book not found: {book}", file=sys.stderr)
        return 2

    if args.build:
        build_simulator(args.env)

    program = Path(args.program).resolve() if args.program else program_path(args.env)
    if not program.exists():
        print(f"Simulator binary not found: {program}", file=sys.stderr)
        print(f"Run: pio run -e {args.env}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="crossink-sim-smoke-") as temp_dir_name:
        temp_root = Path(temp_dir_name)
        simulator_book_path = prepare_fs(temp_root, book)
        if args.library:
            shutil.copy2(book, temp_root / "fs_" / "books" / "main.epub")
            (temp_root / "fs_" / "books" / "reference.txt").write_text(
                "Reference document for A/B position testing.\n\n" * 500, encoding="utf-8"
            )
        if args.checklist:
            checklists = temp_root / "fs_" / "checklists"
            checklists.mkdir()
            (checklists / "01-preshow.md").write_text(
                "- [ ] Radios charged\n# Preshow\n- [x] Sound checked\n"
                "- [ ] Verify the projector image is focused and aligned before opening the house.\n",
                encoding="utf-8",
            )
            (checklists / "02-long.md").write_text(
                "# Equipment preparation\n" + "".join(
                    f"- [ ] Item {i + 1}: Inspect the equipment, confirm its settings, and check the cable connections.\n"
                    for i in range(22)
                ), encoding="utf-8",
            )
            (checklists / "03-notes.md").write_text("# Notes\nOrdinary Markdown remains readable.\n", encoding="utf-8")
            (checklists / "04-too-large.md").write_text("- [ ] Task\n" * 129, encoding="utf-8")
            home_lists = temp_root / "fs_" / "home-checklists"
            home_lists.mkdir()
            (home_lists / "00-ignore.txt").write_text("Not a Markdown file.\n", encoding="utf-8")
            (home_lists / "01-preshow.md").write_text("- [ ] Check radios\n", encoding="utf-8")
            (home_lists / "02-notes.MD").write_text("# Notes\nPlain Markdown.\n", encoding="utf-8")
            empty_lists = temp_root / "fs_" / "home-empty"
            empty_lists.mkdir()
            (empty_lists / "ignore.txt").write_text("Not Markdown.\n", encoding="utf-8")

        env = os.environ.copy()
        # A caller's SD override must never redirect this test to their books.
        env["CROSSPOINT_SIM_SD"] = str(temp_root / "fs_")
        env["CROSSINK_SIMULATOR_SMOKE_TEST"] = "1"
        if args.checklist:
            env["CROSSHATCH_CHECKLIST_SMOKE"] = "1"
        else:
            env.pop("CROSSHATCH_CHECKLIST_SMOKE", None)
        if args.library:
            env["CROSSHATCH_PHASE2_SMOKE"] = "1"
        else:
            env.pop("CROSSHATCH_PHASE2_SMOKE", None)
        env["CROSSINK_SIMULATOR_SMOKE_BOOK"] = simulator_book_path
        env["CROSSINK_SIMULATOR_SMOKE_PAGE_TURNS"] = str(args.page_turns)
        if args.theme:
            env["CROSSINK_SIMULATOR_SMOKE_THEME"] = str(THEMES[args.theme])
        if args.headless:
            env.setdefault("SDL_VIDEODRIVER", "dummy")

        print(f"Running simulator smoke test with isolated fs_: {temp_root / 'fs_'}", flush=True)
        proc = subprocess.run(
            [str(program)],
            cwd=temp_root,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=args.timeout,
        )
        if args.artifacts:
            destination = Path(args.artifacts).resolve()
            destination.mkdir(parents=True, exist_ok=True)
            for screenshot in (temp_root / "fs_").glob("phase[23]-*.bmp"):
                shutil.copy2(screenshot, destination / screenshot.name)

    print(proc.stdout, end="")

    if proc.returncode != 0:
        print(f"Simulator smoke test failed with exit code {proc.returncode}", file=sys.stderr)
        return proc.returncode

    for pattern in CRASH_PATTERNS:
        if pattern in proc.stdout:
            print(f"Simulator smoke test output contained crash pattern: {pattern}", file=sys.stderr)
            return 2

    marker = ("Phase 3 checklist smoke test passed" if args.checklist else
              "Phase 2 library smoke test passed" if args.library else "Simulator smoke test passed")
    if marker not in proc.stdout:
        print("Simulator smoke test did not print its success marker", file=sys.stderr)
        return 2

    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    suite = parser.add_mutually_exclusive_group()
    suite.add_argument("--library", action="store_true", help="Test offline A/B switching, pins, and library views")
    suite.add_argument("--checklist", action="store_true", help="Test Markdown checklist controls and persistence")
    parser.add_argument("--artifacts", help="Copy test screenshots to this directory")
    parser.add_argument("--program", help="Use a simulator binary from a separate build directory (with --no-build)")
    parser.add_argument("--book", default=str(DEFAULT_BOOK), help="EPUB fixture to copy into the isolated simulator fs_")
    parser.add_argument("--env", choices=("simulator", "sticky-simulator", "x4-pro-simulator"), default="simulator",
                        help="PlatformIO simulator environment to build and run")
    parser.add_argument("--timeout", type=int, default=45, help="Seconds before the simulator run is treated as hung")
    parser.add_argument("--page-turns", type=int, default=2, help="Number of EPUB page-forward taps to run")
    parser.add_argument("--theme", choices=sorted(THEMES), help="UI theme to use during the smoke test")
    parser.add_argument("--no-build", dest="build", action="store_false", help="Run the existing simulator binary")
    parser.add_argument("--window", dest="headless", action="store_false", help="Show the SDL window instead of using dummy video")
    parser.set_defaults(build=True, headless=True)
    return parser.parse_args()


def main() -> int:
    return run_smoke(parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
