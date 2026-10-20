#!/usr/bin/env python3
"""Compare two metrics files written by run_embench.py --json.

For every benchmark present in both files the table shows the benchmark()
window: executed instructions, non-dataflow instructions, dataflow share,
mean jmp issue-to-trigger distance, mean ld output reference and in-flight
operands (mean/max), each as "before -> after" with the ratio for the counts.

Usage:
    compare_metrics.py baseline.json new.json [--whole]
"""
import argparse
import json
import math
import sys


def hist_mean(h):
    n = sum(h.values())
    return sum(int(k) * v for k, v in h.items()) / n if n else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("baseline")
    ap.add_argument("new")
    ap.add_argument("--whole", action="store_true",
                    help="compare the whole run instead of the benchmark() window")
    args = ap.parse_args()
    with open(args.baseline) as f:
        base = json.load(f)
    with open(args.new) as f:
        new = json.load(f)
    key = "whole" if args.whole else "window"
    names = sorted(set(base) & set(new))
    if not names:
        sys.exit("no benchmark present in both files")

    # Static code size (bytes of .text, two per instruction) when both files
    # record it (run_embench.py --json does).
    have_static = all("static_text_bytes" in base[n] and "static_text_bytes" in new[n]
                      for n in names)
    print(f"{'benchmark':18} {'instructions':>27} {'ratio':>6} {'non-DF':>27} {'ratio':>6} "
          f"{'DF%':>11} {'jmp':>11} {'ld':>11} {'flight':>13}"
          + (f" {'static bytes':>17} {'ratio':>6}" if have_static else ""))
    ratios, ratios_ndf = [], []
    tb = tn = tbd = tnd = tsb = tsn = 0
    for n in names:
        b, a = base[n][key], new[n][key]
        bi, ai = b["instructions"], a["instructions"]
        bn, an = bi - b["dataflow"], ai - a["dataflow"]
        tb += bi; tn += ai; tbd += bn; tnd += an
        ratios.append(ai / bi if bi else 1.0)
        ratios_ndf.append(an / bn if bn else 1.0)
        line = (f"{n:18} {bi:12,d} -> {ai:12,d} {ai / max(bi, 1):6.2f} {bn:12,d} -> {an:12,d} "
                f"{an / max(bn, 1):6.2f} "
                f"{100 * b['dataflow'] / max(bi, 1):4.1f}->{100 * a['dataflow'] / max(ai, 1):4.1f} "
                f"{hist_mean(b['jmp_trigger']):4.1f}->{hist_mean(a['jmp_trigger']):4.1f} "
                f"{hist_mean(b['load_ref']):4.1f}->{hist_mean(a['load_ref']):4.1f} "
                f"{b['inflight_frame_mean']:4.1f}/{b['inflight_frame_max']}->"
                f"{a['inflight_frame_mean']:4.1f}/{a['inflight_frame_max']}")
        if have_static:
            sb, sn = base[n]["static_text_bytes"], new[n]["static_text_bytes"]
            tsb += sb; tsn += sn
            line += f" {sb:7,d} -> {sn:7,d} {sn / max(sb, 1):6.2f}"
        print(line)
    gm = math.exp(sum(math.log(r) for r in ratios) / len(ratios))
    gmn = math.exp(sum(math.log(r) for r in ratios_ndf) / len(ratios_ndf))
    print(f"{'total':18} {tb:12,d} -> {tn:12,d} {tn / max(tb, 1):6.2f} {tbd:12,d} -> {tnd:12,d} "
          f"{tnd / max(tbd, 1):6.2f}"
          + (f" {'':34} {tsb:7,d} -> {tsn:7,d} {tsn / max(tsb, 1):6.2f}" if have_static else ""))
    print(f"executed bytes: {2 * tb:,d} -> {2 * tn:,d} (two per instruction)")
    print(f"geometric mean of per-benchmark ratios: instructions {gm:.3f}, non-dataflow {gmn:.3f}")


if __name__ == "__main__":
    main()
