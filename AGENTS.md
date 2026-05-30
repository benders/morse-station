# Agent guide

- **Track work in `TODO.md`.** Update it as you go: mark items `[~]` when you
  start and `[x]` when done, and add new items there rather than inventing a
  separate list. Keep the staged structure intact.
- **Reference docs live in `reference/`.** Look there first for datasheets and
  hardware docs. When you find or are given a new one, save it under
  `reference/<board-or-part>/` so it's available later.
- **Work conservatively and incrementally.** Make the smallest change that
  moves one TODO item forward, validate it on hardware before moving on, and
  don't refactor or add scope that wasn't asked for. Prefer proving a thing
  works on one board before wiring up the radio or a second unit.
