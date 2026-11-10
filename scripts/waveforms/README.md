# Single-pass anti-aliasing experiment (UC8279, X4 Pro)

Goal: anti-aliased text pages in **one non-flashing refresh**, instead of the
stock two-step (B/W settle, then a gray overlay ~0.35 s later).

Idea: the controller picks one of four waveform tables per pixel from two
plane bits. Instead of the target tone, the firmware puts an **action** there,
computed from the tone already on the panel and the new tone:
**hold**, **to white**, **to black**, **to gray**. Unchanged pixels idle, as
in a stock partial refresh; only changing pixels are driven. One gray level is
enough: stock drives both of its grays with the same pulse.

Pieces:

- `lib/WaveformActions`: the action computation (host-tested).
- `src/dev/WaveformExperiment.*`: previous/new tone planes in PSRAM.
- Reader: with the experiment on, a text page whose previous tones are known
  goes out as one Direct refresh of action planes; otherwise the stock
  two-step runs and its tones are recorded. Any B/W refresh forgets them.
- `freeink-sdk` (branch `main-custom`): runtime waveform tables per bank,
  refresh timings, and an option to skip the post-Direct full clear.
- `scripts/waveform.py`: table encoder/checker, loader, calibration, timings.

Nothing here persists: a reboot, or `waveform.py reset`, restores stock.

## Table format

49-byte (gray, direct) or 42-byte (settle) tables, groups of 7 bytes:
`[0x01, p1, p2, p3, p4, 0x01, 0x01]`, phase = `(rail << 6) | frames`.
Rail 0 idles; per the driver, rail 1 darkens and rail 2 lightens (calibration
confirms). `0x20` is VCOM, `0x21..0x24` the four pixel classes.
`waveform.py stock` prints the stock tables; stock settle drives a changing
pixel 25 frames, the stock gray pulse is 2 frames (panel revision 0x02/0x03)
or 3 (others). The tool caps any table at 40 frames.

## Session runbook (device on Wi-Fi, Developer Mode on)

All commands from `firmware/`, with `--host <ip>` on each `waveform.py`/`devctl.py` call.

1. **Flash and baseline.**
   `scripts/devctl.py flash` (device unresponsive ~1 min, then reboots).
   `scripts/devctl.py info`: note `display.variant` (gray pulse 2 or 3 frames).
   Open a book, turn a page, then `scripts/waveform.py timings`: expect a
   `settle` (~690 ms refresh) and a `gray` (~350 ms) entry.

2. **Calibrate** (tells which code runs which register, and the polarity).
   `scripts/waveform.py send scripts/waveforms/calibration.json`
   `scripts/waveform.py calibrate`: one full refresh to half black / half
   white, then one Direct pass. In portrait, four bands top to bottom are
   codes 0, 1, 2, 3; the left half started white, the right half black.
   Read each band as "left | right":

   | register (table)        | expected if rail 2 lightens | if reversed               |
   |-------------------------|-----------------------------|---------------------------|
   | 0x21 (rail 2 × 25)      | white \| white              | black \| black            |
   | 0x22 (rail 1 × 25)      | black \| black              | white \| white            |
   | 0x23 (idle)             | white \| black (unchanged)  | white \| black            |
   | 0x24 (rail 1 × 3, idle) | light gray \| black         | white \| dark gray        |

   The band order gives the register for codes 0, 1, 2, 3.
   Restore: `scripts/devctl.py refresh`, `scripts/devctl.py open home`,
   `scripts/waveform.py reset`.

3. **First candidate.**
   `scripts/waveform.py candidate --registers <R0,R1,R2,R3> --gray-frames <2|3> [--rails-reversed] -o scripts/waveforms/candidate-1.json`
   `scripts/waveform.py send scripts/waveforms/candidate-1.json`
   `scripts/waveform.py codes 0 1 2 3`
   `scripts/waveform.py experiment --direct on --full-clear off`
   Open the book: the first page is stock (its tones get recorded), the next
   pages are action pages. Judge on the panel; `waveform.py timings` and
   `devctl.py logs --grep "Action page"` give timing and per-action pixel counts.

4. **Iterate.** Adjust drive length or gray pulse in the spec, `send`, turn
   pages. After each batch: `scripts/devctl.py refresh` (full clean). Stop on
   lasting ghosting.

5. **Back to stock.** `scripts/waveform.py reset` (or reboot).
