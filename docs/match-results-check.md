# Match and results regression

The custom fighter runtime must forget guest pointers when `HSD_CreateMainHeap` destroys the scene heap. Keeping the old costume or texture pointer across match -> results can trigger the game's `ftparts.c` or `tobj.c` assertion loop. That loop does not necessarily make the host process exit with an error.

The runtime now clears the affected caches at that heap boundary. Package identity stays separate from those pointers, allowing results and the next match to load the selected character again. The original CSS restores its token to the custom tile.

## Run

Build the development runtime with `python scripts/build_game.py --output-name melee-mod.exe`. With the example custom Falcon installed in the first added CSS tile, run:

```powershell
python scripts/check_match_results.py --case no-contest --saved-card user/saves/MemoryCardA.raw
python scripts/check_match_results.py --case victory
```

The first command copies the card into its test directory. It exercises two No Contest -> results -> CSS cycles and checks for four fresh custom costume loads. The second command uses P1 custom Falcon and P2 original Falcon, waits for the normal two-minute match to end, then confirms both players on results. These scripts assume the default menu layout and two-minute VS rules; they do not force scene or outcome memory. A copied card with different match rules may need a different input timeline.

Logs, reports, audio and actual Aurora GPU frames go in `build/comparisons/match-results-*`. Checks fail for process errors, timeouts, guest assertions, missing results entries/exits, missing CSS returns, or missing custom costume loads. Inspect the captured screens to confirm the intended scene was rendered; the report alone does not establish visual correctness.

## Verified on 2026-09-08

- `results-repeat-with-save`: two matches, two No Contest results screens, and two returns to the selected custom CSS tile. 8040 host frames, no guest assertions.
- `results-victory-natural`: full timed match, custom Falcon victory pose, original Falcon second place, original results UI, and return to CSS/SSS. 11400 host frames, no guest assertions.
- `results-two-falcons`: original and custom Falcon coexist and animate on No Contest results.

The corresponding `transition-contact.jpg` files contain the visual review captures. The fixed executable SHA256 is `2f53a10168e81dff3f1afe46af3dde3affdb0a753c97656cce2054aa2270706a`.

Use `Run-Melee-Test.cmd` for the fixed development build with normal live controls. The launcher validates XML-derived assets against the user's ISO and does not enable diagnostic input replay.
