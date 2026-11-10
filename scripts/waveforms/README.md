# Single-pass anti-aliasing (UC8279, X4 Pro)

Anti-aliased text pages go out in **one non-flashing refresh** instead of the
stock two-step (a B/W refresh, then a gray overlay about 0.35 s later).
Settings > Reader > Refresh: **Single** (default) or **Two-step**. The setting
is shown on UC8279 panels only.

## How it works

The controller picks one of four waveform tables (registers `0x21..0x24`) per
pixel from two plane bits. Instead of the target tone, the firmware puts an
**action** there, computed from the tone already on the panel and the new
tone: **hold**, **to white**, **to black**, **to gray**. Unchanged pixels
idle, as in a stock partial refresh; only changing pixels are driven. One
gray level is enough: stock drives both of its grays with the same pulse.

| action   | table (phase = rail × frames) |
|----------|-------------------------------|
| hold     | idle 28                       |
| to white | lighten 25, idle 3            |
| to black | darken 25, idle 3             |
| to gray  | darken 25, lighten *g*        |
| VCOM     | idle 28                       |

*g* is the panel's stock gray pulse: 2 frames on panel variants 0x02/0x03
(QY bank), 3 on the others (ZHX bank).

Measured on an X4 Pro (variant 0x68): about 640 ms per page, against about
1.05 s for the stock two-step. Ghosting builds up slowly; the stock clean set
by Refresh Frequency removes it (every 10 pages works well).

Pieces:

- `freeink-sdk`, `Uc8279X4Driver`: the built-in action tables, and Direct
  passes without the full clear when actions are on.
- `lib/WaveformActions`: the action computation (host-tested).
- `src/util/GrayToneTracker.*`: the previous and new tone planes, in PSRAM
  (six 48 KB planes).
- Reader (`EpubReaderActivity`): a text page whose previous tones are known
  goes out as one Direct refresh of action planes. Otherwise (first page,
  pages with images, the periodic clean, or anything else drawn in between)
  the stock two-step runs and its tones are recorded for the next page.

Only variant 0x68 has been checked on hardware. The QY bank is built from
the same rules but untested.

## Experimenting

With Developer Mode on, the dev API can replace the tables at runtime and
report refresh timings; `scripts/waveform.py` drives it. Nothing it sets
persists: a reboot, or `waveform.py reset`, restores the built-in tables.

### Table format

49-byte (gray, direct) or 42-byte (settle) tables, groups of 7 bytes:
`[0x01, p1, p2, p3, p4, 0x01, 0x01]`, phase = `(rail << 6) | frames`.
Rail 0 idles, rail 1 darkens, rail 2 lightens (calibration confirms).
`0x20` is VCOM, `0x21..0x24` the four pixel classes.
`waveform.py stock` prints the stock tables; stock settle drives a changing
pixel 25 frames. The tool caps any table at 40 frames.

### Runbook (device on Wi-Fi, Developer Mode on)

All commands from the repository root, with `--host <ip>` on each
`waveform.py`/`devctl.py` call. The API token is read from `.dev/api-token`
or `$CROSSPOINT_DEV_API_TOKEN`; `devctl.py build` and `flash` build it in.

1. **Flash and baseline.**
   `scripts/devctl.py flash` (device unresponsive about 1 min, then reboots).
   `scripts/devctl.py info`: note `display.variant` (gray pulse 2 or 3 frames).
   Set Refresh to Two-step, open a book, turn a page, then
   `scripts/waveform.py timings`: expect a `settle` (about 690 ms) and a
   `gray` (about 350 ms) entry.

2. **Calibrate** (shows which plane code runs which register, and the
   polarity). Result on variant 0x68: `calibration.json`.
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

   The band order gives the register for codes 0, 1, 2, 3 (on variant 0x68,
   code k runs register 0x21 + k).
   Restore: `scripts/devctl.py refresh`, `scripts/devctl.py open home`,
   `scripts/waveform.py reset`.

3. **Candidate tables.**
   `scripts/waveform.py candidate --registers <R0,R1,R2,R3> --gray-frames <2|3> [--rails-reversed] -o scripts/waveforms/candidate-1.json`
   `scripts/waveform.py send scripts/waveforms/candidate-1.json`
   `scripts/waveform.py codes 0 1 2 3`
   `scripts/waveform.py experiment --direct on --full-clear off`
   Open a book: the first page is stock (its tones get recorded), the next
   pages are single-pass. Judge on the panel; `waveform.py timings` and
   `devctl.py logs --grep "Single-pass page"` give timing and per-action
   pixel counts. `candidate-1.json` is the set the built-in tables match.

4. **Iterate.** Adjust drive length or gray pulse in the spec, `send`, turn
   pages. After each batch: `scripts/devctl.py refresh` (full clean). Stop on
   lasting ghosting.

5. **Back to the built-in tables.** `scripts/waveform.py reset` (or reboot).
