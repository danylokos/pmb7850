from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .battery import load_battery_calibration, synthesize_battery_calibration
from .corpus import analyze_corpus, render_corpus
from .diff import diff_sources, render_block_bytediff
from .identity import (
    generate_identity_bundle,
    generate_overlay_bundle,
    hex_dump,
    verify_checks,
)
from .security import UNKNOWN_KEY1_C55, convert_to_bcd, parse_fsn
from .status_info import load_status_info, status_info_report
from .storage import (
    _parse_int,
    _parse_key1,
    _resolve_want,
    block_class,
    extract_block,
    load_eeprom_source,
    serialize_block,
)

def _use_color(choice: str) -> bool:
    if choice == "always":
        return True
    if choice == "never":
        return False
    return sys.stdout.isatty()


def cmd_list(args: argparse.Namespace) -> int:
    kind, inv = load_eeprom_source(args.source, base=args.eeprom_base,
                                   region_linear_base=args.region_linear_base)
    rows = [inv[i] for i in sorted(inv)]
    if args.json:
        print(json.dumps({
            "source": str(args.source), "kind": kind, "count": len(rows),
            "blocks": [serialize_block(block) for block in rows],
        }, indent=2))
        return 0
    print(f"source : {args.source}  ({kind}, {len(rows)} blocks)")
    print(f"{'id':>6} {'eep':>6}  {'linear':>10} {'file':>10} {'len':>6} "
          f"{'mem':>3} {'ver':>3} {'marker':>6}  {'class':<9} name")
    for b in rows:
        lin = f"{b.linear:#010x}" if b.linear is not None else "-"
        fo = f"{b.file_off:#010x}" if b.file_off is not None else "-"
        marker = f"{b.marker:04x}" if b.marker is not None else "-"
        print(f"{b.block_id:#06x} {b.block_id:6d}  {lin:>10} {fo:>10} {b.length:6d} "
              f"{str(b.memory_class) if b.memory_class is not None else '-':>3} "
              f"{str(b.version) if b.version is not None else '-':>3} {marker:>6}  "
              f"{block_class(b.block_id):<9} {b.name}")
    return 0


def cmd_extract(args: argparse.Namespace) -> int:
    kind, inv = load_eeprom_source(args.source, base=args.eeprom_base,
                                   region_linear_base=args.region_linear_base)
    key1 = args.key1 if args.key1 is not None else UNKNOWN_KEY1_C55
    want = _resolve_want(args.block, inv, args.verify)

    results: list[dict] = []
    dec_cipher: dict[int, bytes] = {}
    for bid in want:
        info = extract_block(inv, bid, args.fsn, args.imei, key1)
        if info["class"] == "cipher":
            dec_cipher[bid] = bytes.fromhex(info["plaintext_hex"])
        results.append(info)

    checks = None
    if args.verify:
        checks = verify_checks(dec_cipher[0x1390], dec_cipher[0x13D5])

    if args.json:
        out = {
            "source": str(args.source), "kind": kind,
            "fsn": f"{args.fsn:#010x}" if args.fsn is not None else None,
            "imei": args.imei, "blocks": results,
        }
        if checks is not None:
            out["checks"] = [{"region": r, "sum": s, "xor": x} for r, s, x in checks]
        print(json.dumps(out, indent=2))
        return 0

    print(f"source  : {args.source}  ({kind})")
    if args.fsn is not None:
        print(f"fsn     : {args.fsn:#010x}")
    if args.imei is not None:
        print(f"imei    : {args.imei}  (BCD {convert_to_bcd(args.imei).hex()})")
    if checks is not None:
        print("verify  : recomputed DD2476 sum/xor pairs (compare to firmware-recorded values)")
        for region_name, s, x in checks:
            print(f"          {region_name:<13} sum={s:02X} xor={x:02X}")
    print()
    for info in results:
        bid = info["id"]
        data = bytes.fromhex(info.get("plaintext_hex") or info.get("raw_hex") or "")
        kindlabel = "decrypted" if info["class"] == "cipher" else info["class"]
        addr = ""
        if info.get("linear"):
            addr = f"linear {info['linear']}, file {info['file_off']}, "
        print(f"=== {bid} ({bid:#06x}, {info['class']}, {addr}"
              f"{info['length']} bytes, {kindlabel}) ===")
        print(f"  memory={info['memory_class']} version={info['version']} "
              f"marker={info['marker']} directory={info['dir_off']}")
        if info["class"] in ("scramble", "imei-companion"):
            print(f"  imei: {'<empty/0xFF>' if info.get('empty') else info.get('imei', '?')}")
        if info["class"] == "plaintext" and info.get("simlock"):
            print("  (SIMLOCK signature present)")
        print(hex_dump(data))
        print()
    return 0


def cmd_diff(args: argparse.Namespace) -> int:
    kind_a, inv_a = load_eeprom_source(args.a, base=args.eeprom_base,
                                       region_linear_base=args.region_linear_base)
    kind_b, inv_b = load_eeprom_source(args.b, base=args.eeprom_base,
                                       region_linear_base=args.region_linear_base)

    # Single-block byte-level view.
    if args.block is not None:
        bid = _resolve_want(args.block, {**inv_a, **inv_b}, False)[0]
        in_a, in_b = bid in inv_a, bid in inv_b
        if args.json:
            pa = inv_a[bid].payload if in_a else b""
            pb = inv_b[bid].payload if in_b else b""
            n = max(len(pa), len(pb))
            print(json.dumps({
                "block": bid, "in_a": in_a, "in_b": in_b,
                "version_a": inv_a[bid].version if in_a else None,
                "version_b": inv_b[bid].version if in_b else None,
                "memory_class_a": inv_a[bid].memory_class if in_a else None,
                "memory_class_b": inv_b[bid].memory_class if in_b else None,
                "marker_a": inv_a[bid].marker if in_a else None,
                "marker_b": inv_b[bid].marker if in_b else None,
                "bytes": [{"off": i,
                           "a": pa[i] if i < len(pa) else None,
                           "b": pb[i] if i < len(pb) else None,
                           "equal": i < len(pa) and i < len(pb) and pa[i] == pb[i]}
                          for i in range(n)],
            }, indent=2))
            return 0
        print(f"A: {args.a}  ({kind_a})")
        print(f"B: {args.b}  ({kind_b})")
        print(f"block {bid} ({bid:#06x}, {block_class(bid)})")
        if not in_a or not in_b:
            only = "A" if in_a else "B"
            block = (inv_a if in_a else inv_b).get(bid)
            if block is None:
                print(f"  block {bid} present in neither source")
                return 1
            print(f"  present only in {only}; dumping that side:")
            print(hex_dump(block.payload))
            return 0
        print(f"  metadata A: memory={inv_a[bid].memory_class} "
              f"version={inv_a[bid].version} marker={inv_a[bid].marker}")
        print(f"  metadata B: memory={inv_b[bid].memory_class} "
              f"version={inv_b[bid].version} marker={inv_b[bid].marker}")
        print(render_block_bytediff(inv_a[bid].payload, inv_b[bid].payload,
                                    color=_use_color(args.color)))
        return 0

    # Whole-inventory diff.
    result = diff_sources(inv_a, inv_b)
    if args.json:
        print(json.dumps({
            "a": str(args.a), "b": str(args.b), "kind_a": kind_a, "kind_b": kind_b,
            "common": result.common, "equal": result.equal,
            "differ": [{"id": d.block_id, "len_a": d.len_a, "len_b": d.len_b,
                        "first_diff": d.first_diff,
                        "version_a": d.version_a, "version_b": d.version_b,
                        "memory_class_a": d.memory_class_a,
                        "memory_class_b": d.memory_class_b}
                       for d in result.differ],
            "only_in_a": result.only_in_a, "only_in_b": result.only_in_b,
        }, indent=2))
        return 0

    print(f"A: {args.a}  ({kind_a}, {len(inv_a)} blocks)")
    print(f"B: {args.b}  ({kind_b}, {len(inv_b)} blocks)")
    print(f"stats: common={result.common} equal={result.equal_count} "
          f"differ={len(result.differ)} A-only={len(result.only_in_a)} "
          f"B-only={len(result.only_in_b)}")
    if result.equal:
        print(f"\nequal ({len(result.equal)}): "
              + ", ".join(str(i) for i in result.equal))
    if result.differ:
        print(f"\ndiffer ({len(result.differ)}): "
              + ", ".join(str(d.block_id) for d in result.differ))
    if result.only_in_a:
        print(f"\nonly in A ({len(result.only_in_a)}): "
              + ", ".join(str(i) for i in result.only_in_a))
    if result.only_in_b:
        print(f"\nonly in B ({len(result.only_in_b)}): "
              + ", ".join(str(i) for i in result.only_in_b))
    return 0


def cmd_generate(args: argparse.Namespace) -> int:
    bundle = (
        generate_identity_bundle(args.imei, args.fsn)
        if args.identity_only
        else generate_overlay_bundle(args.imei, args.fsn)
    )
    rendered = json.dumps(bundle, indent=2, sort_keys=True) + "\n"
    args.output.write_text(rendered, encoding="utf-8")
    return 0


def cmd_corpus(args: argparse.Namespace) -> int:
    report = analyze_corpus(args.sources, args.block)
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(render_corpus(report), end="")
    return 0 if report["summary"]["parsed"] else 1


def _battery_report(source: Path, kind: str, block, calibration) -> dict:
    return {
        "source": str(source),
        "kind": kind,
        "block": {
            "id": block.block_id,
            "length": block.length,
            "linear": block.linear,
            "file_offset": block.file_off,
            "directory_offset": block.dir_off,
            "version": block.version,
            "marker": block.marker,
        },
        "calibration": calibration.as_dict(),
    }


def cmd_battery_inspect(args: argparse.Namespace) -> int:
    kind, block, calibration = load_battery_calibration(
        args.source,
        eeprom_base=args.eeprom_base,
        region_linear_base=args.region_linear_base,
    )
    report = _battery_report(args.source, kind, block, calibration)
    if args.json:
        print(json.dumps(report, indent=2))
        return 0
    print(f"source       : {args.source} ({kind})")
    print(f"block        : 67, {block.length} bytes")
    print(f"linear       : {block.linear:#x}" if block.linear is not None
          else "linear       : -")
    print(f"file offset  : {block.file_off:#x}" if block.file_off is not None
          else "file offset  : -")
    print(f"directory    : {block.dir_off:#x}" if block.dir_off is not None
          else "directory    : -")
    print(f"version      : {block.version}")
    print(f"marker       : {block.marker:#06x}" if block.marker is not None
          else "marker       : -")
    print(f"low endpoint : raw {calibration.low_raw}, {calibration.low_mv} mV")
    print(f"high endpoint: raw {calibration.high_raw}, {calibration.high_mv} mV")
    print(f"span         : {calibration.span_mv} mV")
    print(
        f"TBAT adjust  : {calibration.tbat.scale_percent}% scale, "
        f"offset {calibration.tbat.offset}"
    )
    print(
        f"TENV adjust  : {calibration.tenv.scale_percent}% scale, "
        f"offset {calibration.tenv.offset}"
    )
    print(
        f"reserved     : {calibration.reserved.scale_percent}% scale, "
        f"offset {calibration.reserved.offset}"
    )
    return 0


def cmd_battery_synthesize(args: argparse.Namespace) -> int:
    block, calibration = synthesize_battery_calibration(
        args.source,
        args.output,
        low_raw=args.low_raw,
        low_mv=args.low_mv,
        high_raw=args.high_raw,
        high_mv=args.high_mv,
        eeprom_base=args.eeprom_base,
        region_linear_base=args.region_linear_base,
    )
    print(
        "warning: physical handset calibration requires measured ADC points",
        file=sys.stderr,
    )
    print(
        f"wrote {args.output}: block 67 at {block.file_off:#x}, "
        f"raw/mV {calibration.low_raw}/{calibration.low_mv} -> "
        f"{calibration.high_raw}/{calibration.high_mv}"
    )
    return 0


def cmd_status_info(args: argparse.Namespace) -> int:
    kind, block, info = load_status_info(
        args.source,
        eeprom_base=args.eeprom_base,
        region_linear_base=args.region_linear_base,
    )
    report = status_info_report(args.source, kind, block, info)
    if args.json:
        print(json.dumps(report, indent=2))
        return 0
    print(f"source          : {args.source} ({kind})")
    print(f"block           : 5005, {block.length} bytes")
    print(f"linear          : {block.linear:#x}" if block.linear is not None else
          "linear          : -")
    print(f"file offset     : {block.file_off:#x}" if block.file_off is not None else
          "file offset     : -")
    print(f"directory       : {block.dir_off:#x}" if block.dir_off is not None else
          "directory       : -")
    print(f"memory/version  : {block.memory_class}/{block.version}")
    print(f"raw             : {block.payload.hex()}")
    print(f"production date : {info.production_date_text or '<invalid>'}")
    print(f"Standard Map/SW : {info.standard_map_sw}")
    print(f"D-Map/Provider  : {info.d_map_provider_text}")
    print(f"variant         : {info.variant}")
    print(f"ignored         : +0D={info.reserved_0d:02X}, +14.low={info.filler_14_low:X}")
    print("opaque          : +00..+0A, +15..+3F")
    return 0


def _add_region_opts(p: argparse.ArgumentParser) -> None:
    p.add_argument("--eeprom-base", type=_parse_int, default=None,
                   help="file offset of the EEPROM region start (auto-detected if omitted)")
    p.add_argument("--region-linear-base", type=_parse_int, default=None,
                   help="linear base of the EEPROM region (auto-derived from the directory "
                        "if omitted; e.g. C55/A55 0xFA0000, M55 0xFC0000)")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.siemens_tools eeprom",description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_list = sub.add_parser("list", help="enumerate every block in a source")
    p_list.add_argument("source", type=Path, help="a flash dump / EEPROM slice, or a .map file")
    p_list.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    _add_region_opts(p_list)
    p_list.set_defaults(func=cmd_list)

    p_extract = sub.add_parser("extract", help="recover one/some blocks' bytes")
    p_extract.add_argument("source", type=Path,
                           help="a flash dump / EEPROM slice, or a .map file")
    p_extract.add_argument("--block", default=None,
                           help="block to extract: an EEP number (5008), hex id (0x1390), "
                                "'5009'/'5008'/'5077', 'cipher' (5008+5077), or 'all' "
                                "(default: cipher)")
    p_extract.add_argument("--fsn", type=parse_fsn,
                           help="8-digit hexadecimal FSN used by cipher blocks")
    p_extract.add_argument("--imei", help="14-digit IMEI (folded to BCD for the cipher key)")
    p_extract.add_argument("--key1", type=_parse_key1, default=None,
                           help="override the cipher key row: a proven model-ID-7 alias "
                                "(including M55/S55) or 16 u32 words")
    p_extract.add_argument("--verify", action="store_true",
                           help="recompute the three DD2476 sum/XOR pairs from plaintext")
    p_extract.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    _add_region_opts(p_extract)
    p_extract.set_defaults(func=cmd_extract)

    p_diff = sub.add_parser("diff", help="compare two EEPROM sources by block id")
    p_diff.add_argument("a", type=Path, help="first source (dump/slice or .map)")
    p_diff.add_argument("b", type=Path, help="second source (dump/slice or .map)")
    p_diff.add_argument("--block", default=None,
                        help="show a byte-level side-by-side diff of just this block "
                             "(matching bytes green, differing red)")
    p_diff.add_argument("--color", choices=("auto", "always", "never"), default="auto",
                        help="colorize the --block byte diff (default: auto = on if a TTY)")
    p_diff.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    _add_region_opts(p_diff)
    p_diff.set_defaults(func=cmd_diff)

    p_generate = sub.add_parser(
        "generate", help="create a deterministic unlocked model-ID-7 identity overlay"
    )
    p_generate.add_argument("--imei", required=True,
                            help="exactly 14 decimal IMEI digits; check digit is derived")
    p_generate.add_argument("--fsn", type=parse_fsn, required=True,
                            help="exactly 8 hexadecimal FSN digits")
    p_generate.add_argument("--output", type=Path, required=True,
                            help="output bundle JSON path")
    p_generate.add_argument(
        "--identity-only", action="store_true",
        help="omit optional battery-calibration block 67",
    )
    p_generate.set_defaults(func=cmd_generate)

    p_corpus = sub.add_parser(
        "corpus", help="cluster selected blocks across files and directory trees"
    )
    p_corpus.add_argument(
        "sources", nargs="+", type=Path,
        help="one or more .bin/.map files or directories to scan recursively",
    )
    p_corpus.add_argument(
        "--block", action="append", type=_parse_int, required=True,
        help="EEPROM block id to analyze; repeat for multiple blocks",
    )
    p_corpus.add_argument(
        "--json", action="store_true", help="emit machine-readable JSON"
    )
    p_corpus.set_defaults(func=cmd_corpus)

    p_status_info = sub.add_parser(
        "status-info", help="decode the live Status fields in EEPROM block 5005"
    )
    p_status_info.add_argument("source", type=Path)
    p_status_info.add_argument(
        "--json", action="store_true", help="emit machine-readable JSON"
    )
    _add_region_opts(p_status_info)
    p_status_info.set_defaults(func=cmd_status_info)

    p_battery = sub.add_parser(
        "battery", help="inspect or synthesize EEPROM battery calibration"
    )
    battery_sub = p_battery.add_subparsers(dest="battery_cmd", required=True)
    p_battery_inspect = battery_sub.add_parser(
        "inspect", help="decode active EEPROM block 67"
    )
    p_battery_inspect.add_argument("source", type=Path)
    p_battery_inspect.add_argument(
        "--json", action="store_true", help="emit machine-readable JSON"
    )
    _add_region_opts(p_battery_inspect)
    p_battery_inspect.set_defaults(func=cmd_battery_inspect)

    p_battery_synthesize = battery_sub.add_parser(
        "synthesize", help="replace block 67 ADC/voltage endpoints in a dump"
    )
    p_battery_synthesize.add_argument("source", type=Path)
    p_battery_synthesize.add_argument("--low-raw", type=int, required=True)
    p_battery_synthesize.add_argument("--low-mv", type=int, required=True)
    p_battery_synthesize.add_argument("--high-raw", type=int, required=True)
    p_battery_synthesize.add_argument("--high-mv", type=int, required=True)
    p_battery_synthesize.add_argument("--output", type=Path, required=True)
    _add_region_opts(p_battery_synthesize)
    p_battery_synthesize.set_defaults(func=cmd_battery_synthesize)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except (ValueError, KeyError) as exc:
        parser.error(str(exc))
