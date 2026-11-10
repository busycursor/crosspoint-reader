#!/usr/bin/env python3
"""Waveform experiments for the UC8279 X4 Pro panel (see the driver's
setExperimentLut and src/dev/DevApi.cpp /dev/display/*).

Table format (decoded from Uc8279X4Driver.cpp): a 49-byte gray/direct table or
a 42-byte settle table is 7 or 6 groups of 7 bytes:
    [0x01, phase1, phase2, phase3, phase4, 0x01, 0x01]
Each phase byte is (rail << 6) | frames: rail 0 = ground (idle), 1 and 2 are
the two drive rails; frames 0..63. Polarity, read from the driver (gray pixels
sit on black in the B/W base and stock lightens them with rail 2): rail 1
darkens, rail 2 lightens. The calibration pattern confirms it on the panel.
Register 0x20 is VCOM; 0x21..0x24 are the four pixel classes picked by each
pixel's two plane bits. Unused groups are zero.

Spec files (JSON) describe tables as phase lists:
    {"bank": "direct",
     "tables": {"0x20": [[0, 25]], "0x21": [[1, 6], [1, 6]], ...}}

  waveform.py stock                         decoded stock tables, as specs
  waveform.py check SPEC                    validate and show bytes, frames, balance
  waveform.py send SPEC [--host IP]         validate, then load onto the device
  waveform.py reset [--host IP]             back to stock tables, experiment off
  waveform.py experiment --direct on|off [--full-clear on|off] [--host IP]
  waveform.py timings [--host IP]           recent refresh phase timings
  waveform.py calibrate [--host IP]         show the four-band code pattern (see README)
  waveform.py codes HOLD WHITE BLACK GRAY   2-bit plane code for each action
  waveform.py candidate --registers R0,R1,R2,R3 --gray-frames N [--rails-reversed] [-o SPEC]
                                            action tables for codes 0..3 (hold, white, black,
                                            gray), placed on the registers calibration found
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
import devctl  # noqa: E402

DRIVER = devctl.FIRMWARE / "freeink-sdk/libs/display/FreeInkDisplay/src/driver/Uc8279X4Driver.cpp"
BANK_BYTES = {"overlay": 49, "direct": 49, "settle": 42}
GROUP = 7
RAILS = {0: "idle", 1: "darken", 2: "lighten"}  # polarity per the driver; calibration confirms
# Panel care: no table may run longer than this many frames. The stock settle
# drives changing pixels for 25 frames and the stock AA pass for ~6; the cap
# leaves room for reset-then-set tables without exceeding stock drive by much.
MAX_TABLE_FRAMES = 40


class SpecError(ValueError):
    pass


def encode_table(phases: list, nbytes: int) -> bytes:
    """Pack [rail, frames] phases into the controller's grouped layout."""
    groups = nbytes // GROUP
    if len(phases) > groups * 4:
        raise SpecError(f"{len(phases)} phases; at most {groups * 4} fit in {nbytes} bytes")
    out = bytearray(nbytes)
    for g in range(groups):
        chunk = phases[g * 4:(g + 1) * 4]
        if not chunk:
            break
        base = g * GROUP
        out[base] = 0x01
        for i, (rail, frames) in enumerate(chunk):
            if rail not in RAILS:
                raise SpecError(f"rail {rail} not in {sorted(RAILS)} (rail 3 is never used by stock)")
            if not 0 <= frames <= 63:
                raise SpecError(f"frames {frames} outside 0..63")
            out[base + 1 + i] = (rail << 6) | frames
        out[base + 5] = 0x01
        out[base + 6] = 0x01
    return bytes(out)


def decode_table(data: bytes) -> list:
    phases = []
    for g in range(len(data) // GROUP):
        grp = data[g * GROUP:(g + 1) * GROUP]
        if not any(grp[1:5]):
            continue
        for b in grp[1:5]:
            if b:
                phases.append([b >> 6, b & 0x3F])
    return phases


def stats(phases: list) -> dict:
    frames = sum(f for _, f in phases)
    net = sum(f if r == 1 else -f if r == 2 else 0 for r, f in phases)
    return {"frames": frames, "net_vsh_minus_vsl": net}


def register(key: str) -> int:
    reg = int(key, 16) if str(key).lower().startswith("0x") else int(key)
    if reg < 0x20:
        reg += 0x20
    if not 0x20 <= reg <= 0x24:
        raise SpecError(f"table key {key}: registers are 0x20..0x24")
    return reg


def load_spec(path: str) -> tuple[str, dict]:
    spec = json.loads(Path(path).read_text())
    bank = spec.get("bank")
    if bank not in BANK_BYTES:
        raise SpecError(f"bank must be one of {sorted(BANK_BYTES)}")
    tables = {}
    for key, phases in spec["tables"].items():
        reg = register(key)
        data = encode_table(phases, BANK_BYTES[bank])
        st = stats(phases)
        if st["frames"] > MAX_TABLE_FRAMES:
            raise SpecError(f"{hex(reg)}: {st['frames']} frames exceeds the {MAX_TABLE_FRAMES}-frame panel-care cap")
        tables[reg] = (phases, data, st)
    if bank == "direct" and set(tables) != {0x20, 0x21, 0x22, 0x23, 0x24}:
        # A missing Direct table falls back to the flashing four-tone quality bank.
        raise SpecError("a direct spec must define all five tables 0x20..0x24")
    return bank, tables


def stock_specs() -> dict:
    """Pull the stock tables out of the driver source."""
    src = DRIVER.read_text()
    specs = {}

    def grab(name: str, nbytes: int, has_cmd_field: bool) -> dict:
        m = re.search(name + r"\s*(?:\[[^\]]*\])+\s*=\s*\{(.*?)\n\};", src, re.S)
        if not m:
            raise SystemExit(f"could not find {name} in {DRIVER}")
        block = m.group(1)
        # Rows are either {0x2N, 0x.., ...} or {0x2N, {0x.., ...}} (GrayLut).
        rows = re.findall(r"\{\s*(0x2[0-4])\s*,\s*\{([^{}]*)\}\s*\}", block) if has_cmd_field else []
        if not rows:
            rows = [(r.split(",")[0], r.split(",", 1)[1]) for r in re.findall(r"\{([^{}]*)\}", block)]
        tables = {}
        for reg, body in rows:
            nums = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", body)]
            nums = (nums + [0] * nbytes)[:nbytes]
            tables[hex(int(reg, 16))] = decode_table(bytes(nums))
        return tables

    specs["overlay_qy"] = {"bank": "overlay", "tables": grab("kXtfAa02", 49, True)}
    specs["overlay_zhx"] = {"bank": "overlay", "tables": grab("kXtfAa68", 49, True)}
    specs["settle"] = {"bank": "settle", "tables": grab("kXtfPreBwMid", 42, True)}
    return specs


def client(args) -> devctl.Client:
    return devctl.make_client(SimpleNamespace(host=args.host, serial=args.serial))


def cmd_stock(args) -> None:
    for name, spec in stock_specs().items():
        print(f"== {name} ({spec['bank']})")
        for reg, phases in spec["tables"].items():
            st = stats(phases)
            print(f"  {reg}: {phases}  frames={st['frames']} net={st['net_vsh_minus_vsl']:+d}")
    if args.write:
        out = Path(args.write)
        out.mkdir(parents=True, exist_ok=True)
        for name, spec in stock_specs().items():
            (out / f"stock-{name}.json").write_text(json.dumps(spec, indent=1) + "\n")
        print(f"wrote stock specs to {out}")


def cmd_check(args) -> None:
    bank, tables = load_spec(args.spec)
    print(f"bank {bank}: {len(tables)} table(s), all within limits")
    for reg, (phases, data, st) in sorted(tables.items()):
        print(f"  {hex(reg)} frames={st['frames']:2d} net={st['net_vsh_minus_vsl']:+3d}  {data[:21].hex(' ')} ...")
    lengths = {st["frames"] for _, _, st in tables.values()}
    if len(lengths) > 1:
        print(f"  note: tables end on different frames ({sorted(lengths)}); the refresh lasts as long as the longest")


def cmd_send(args) -> None:
    bank, tables = load_spec(args.spec)
    payload = {"bank": bank, "tables": {str(reg - 0x20): list(data) for reg, (_, data, _) in tables.items()}}
    print(json.dumps(client(args).json("POST", "/dev/display/waveform", payload)))


def cmd_reset(args) -> None:
    print(json.dumps(client(args).json("DELETE", "/dev/display/waveform")))


def cmd_experiment(args) -> None:
    payload = {}
    if args.direct:
        payload["direct"] = args.direct == "on"
    if args.full_clear:
        payload["fullClear"] = args.full_clear == "on"
    print(json.dumps(client(args).json("POST", "/dev/display/experiment", payload)))


def cmd_timings(args) -> None:
    doc = client(args).json("GET", "/dev/display/timings")
    print(f"experiment direct={doc['direct']}")
    for t in doc["refreshes"]:
        print(f"  {t['pass']:7} at {t['at_ms']:>9} ms  stream {t['stream_us']/1000:6.1f}  lut {t['lut_us']/1000:5.1f}  "
              f"power-on {t['power_on_us']/1000:6.1f}  refresh {t['refresh_us']/1000:7.1f}  "
              f"power-off {t['power_off_us']/1000:5.1f}  total {t['total_us']/1000:7.1f} ms")


def cmd_calibrate(args) -> None:
    print(json.dumps(client(args).json("POST", "/dev/display/calibrate")))


def cmd_codes(args) -> None:
    codes = {"hold": args.hold, "white": args.white, "black": args.black, "gray": args.gray}
    print(json.dumps(client(args).json("POST", "/dev/display/experiment", {"codes": codes})))


DRIVE = 25  # frames: the stock settle drive for a changing pixel


def candidate_spec(registers: list[int], gray_frames: int, rails_reversed: bool = False) -> dict:
    """Action tables for codes 0..3 = hold, to white, to black, to gray. Gray
    is reached from any tone by driving to black, then lightening for the
    stock gray pulse length (2 or 3 frames by panel revision). All tables end
    on the same frame; VCOM idles throughout."""
    total = DRIVE + gray_frames
    actions = [
        [[0, total]],                                   # hold
        [[2, DRIVE], [0, gray_frames]],                 # to white
        [[1, DRIVE], [0, gray_frames]],                 # to black
        [[1, DRIVE], [2, gray_frames]],                 # to gray
    ]
    if rails_reversed:  # calibration showed rail 1 lightening
        actions = [[[{1: 2, 2: 1}.get(r, r), f] for r, f in a] for a in actions]
    tables = {"0x20": [[0, total]]}
    for code, reg in enumerate(registers):
        tables[hex(reg)] = actions[code]
    return {"bank": "direct", "codes": {"hold": 0, "white": 1, "black": 2, "gray": 3}, "tables": tables}


def cmd_candidate(args) -> None:
    regs = [int(r, 16) for r in args.registers.split(",")]
    if sorted(regs) != [0x21, 0x22, 0x23, 0x24]:
        sys.exit("--registers needs 0x21..0x24 each once, in code order 0,1,2,3")
    spec = candidate_spec(regs, args.gray_frames, args.rails_reversed)
    text = json.dumps(spec, indent=1) + "\n"
    if args.output:
        Path(args.output).write_text(text)
        print(f"wrote {args.output}")
    else:
        print(text)


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=None)
    p.add_argument("--serial", default=None, metavar="PORT")
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("stock")
    s.add_argument("--write", help="also write stock specs into this folder")
    s = sub.add_parser("check")
    s.add_argument("spec")
    s = sub.add_parser("send")
    s.add_argument("spec")
    sub.add_parser("reset")
    s = sub.add_parser("experiment")
    s.add_argument("--direct", choices=["on", "off"])
    s.add_argument("--full-clear", choices=["on", "off"])
    sub.add_parser("timings")
    sub.add_parser("calibrate")
    s = sub.add_parser("codes")
    for name in ("hold", "white", "black", "gray"):
        s.add_argument(name, type=int, choices=range(4))
    s = sub.add_parser("candidate")
    s.add_argument("--registers", required=True, help="register for codes 0,1,2,3, e.g. 0x23,0x21,0x22,0x24")
    s.add_argument("--gray-frames", type=int, choices=range(1, 9), required=True)
    s.add_argument("--rails-reversed", action="store_true", help="calibration showed rail 1 lightening")
    s.add_argument("-o", "--output")
    args = p.parse_args()
    try:
        {"stock": cmd_stock, "check": cmd_check, "send": cmd_send, "reset": cmd_reset,
         "experiment": cmd_experiment, "timings": cmd_timings, "calibrate": cmd_calibrate,
         "codes": cmd_codes, "candidate": cmd_candidate}[args.cmd](args)
    except (SpecError, devctl.ApiError) as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
