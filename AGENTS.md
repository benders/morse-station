# Agent guide

- **Track work in [GitHub Issues](https://github.com/benders/morse-station/issues).**
  Open work lives in issues — comment progress and close them when done, and
  file a new issue rather than inventing a separate list. Completed work is
  archived in `DONE.md`. Use labels (`rak4631`, `cardputer`, `radio`,
  `instructor-alert`, `idea`, plus the built-ins) to keep the areas sorted.
- **Reference docs live in `reference/`.** Look there first for datasheets and
  hardware docs. When you find or are given a new one, save it under
  `reference/<board-or-part>/` so it's available later.
- **Define pin mappings in `platformio.ini`, not in code.** Hardware pin
  assignments belong in the env `build_flags` (`-DPIN_*`); `src/pins.h` should
  only *consume* those defines (with `#ifndef` fallbacks for standalone
  compilation). This keeps the wiring map in one editable place per board.
- **Work conservatively and incrementally.** Make the smallest change that
  moves one issue forward, validate it on hardware before moving on, and
  don't refactor or add scope that wasn't asked for. Prefer proving a thing
  works on one board before wiring up the radio or a second unit.
