#!/usr/bin/env python3
"""Markdown tables of the ISA and compiler metrics of an Embench run.

    metrics_report.py metrics.json [--rv32 rv32.json] [--whole]

metrics.json is written by run_embench.py --metrics --profile --json (the
static view under "static" needs --profile for the execution-weighted
figures); rv32.json by rv32_metrics.py --json for the same benchmarks.
Every table has one row per benchmark and a **pooled** row computed from
the summed counts and histograms (means, shares and quantiles over all
benchmarks' events, not averages of the per-benchmark figures); the
code-size table, which compares the two ISAs, ends in a **geometric
mean** row over the benchmarks instead, with the pooled sums in a
sentence after it.

Tables: code size, dynamic instruction mix, static instruction mix,
operand lifetime, control-flow lifetime, jump distance quantiles (trigger
field as written and issue-to-trigger as executed), memory traffic, load
distance quantiles (output reference as written and load-to-use as
executed), operands in flight.
"""
import argparse
import json
import math
import sys

CLASSES = ("control", "dataflow", "alu", "cast", "pick", "const", "memory", "frame", "stack", "trap")


def add_hist(acc, h):
    for k, v in h.items():
        acc[int(k)] = acc.get(int(k), 0) + v


def h_n(h):
    return sum(h.values())


def h_mean(h):
    n = h_n(h)
    return sum(int(k) * v for k, v in h.items()) / n if n else 0.0


def h_quantile(h, q):
    n = h_n(h)
    if not n:
        return 0
    acc = 0
    for k, v in sorted(h.items(), key=lambda kv: int(kv[0])):
        acc += v
        if acc >= q * n:
            return int(k)
    return 0


def h_max(h):
    return max((int(k) for k in h), default=0)


QUANTILE_HDR = ["mean", "p10", "p25", "median", "p75", "p90", "p99", "max"]


def quantile_row(h):
    """mean, p10, p25, median, p75, p90, p99, max of a {value: count} histogram."""
    if not h_n(h):
        return ["-"] * len(QUANTILE_HDR)
    return [f"{h_mean(h):.1f}"] + [h_quantile(h, q) for q in (0.1, 0.25, 0.5, 0.75, 0.9, 0.99)] + [h_max(h)]


def h_share(h, pred):
    n = h_n(h)
    return 100.0 * sum(v for k, v in h.items() if pred(int(k))) / n if n else 0.0


def pct(a, b):
    return f"{100.0 * a / b:.1f}%" if b else "-"


def per_k(a, b):
    return f"{1000.0 * a / b:.1f}" if b else "-"


def ratio(a, b):
    return f"{a / b:.2f}" if b else "-"


def gmean(xs):
    xs = [x for x in xs if x > 0]
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else 0.0


def table(header, rows, align=None):
    out = ["| " + " | ".join(header) + " |",
           "|" + "|".join(("---:" if i else "---") for i in range(len(header))) + "|"]
    for r in rows:
        out.append("| " + " | ".join(str(c) for c in r) + " |")
    return "\n".join(out)


def pooled(scopes, static):
    """One pseudo-benchmark whose counts and histograms are the sums."""
    p = {"instructions": 0, "dataflow": 0, "produced": 0, "discards": 0, "forwards": 0,
         "loads": 0, "load_bytes": 0, "stores": 0, "store_bytes": 0, "stack_loads": 0,
         "stack_load_bytes": 0, "stack_stores": 0, "stack_store_bytes": 0, "jmp_issued": 0,
         "jmp_taken": 0, "jmp_not_taken": 0, "jmp_overridden": 0, "call_depth_max": 0,
         "steps": 0, "inflight_frame_sum": 0.0,
         "inflight_all_sum": 0.0, "inflight_frame_max": 0, "inflight_all_max": 0,
         "opcodes": {}}
    hists = ("value_ref", "load_dynamic", "load_ref", "jmp_trigger", "jmp_dynamic",
             "call_trigger", "call_dynamic", "ret_trigger", "ret_dynamic")
    for h in hists:
        p[h] = {}
    for w in scopes:
        for k in ("instructions", "dataflow", "produced", "discards", "forwards", "loads",
                  "load_bytes", "stores", "store_bytes", "stack_loads", "stack_load_bytes",
                  "stack_stores", "stack_store_bytes", "jmp_issued", "jmp_taken", "jmp_not_taken",
                  "jmp_overridden", "steps"):
            p[k] += w.get(k, 0)
        p["call_depth_max"] = max(p["call_depth_max"], w.get("call_depth_max", 0))
        p["inflight_frame_max"] = max(p["inflight_frame_max"], w["inflight_frame_max"])
        p["inflight_all_max"] = max(p["inflight_all_max"], w["inflight_all_max"])
        p["inflight_frame_sum"] += w["inflight_frame_mean"] * w["steps"]
        p["inflight_all_sum"] += w["inflight_all_mean"] * w["steps"]
        for h in hists:
            add_hist(p[h], w[h])
        for k, v in w["opcodes"].items():
            p["opcodes"][k] = p["opcodes"].get(k, 0) + v
    p["inflight_frame_mean"] = p["inflight_frame_sum"] / p["steps"] if p["steps"] else 0.0
    p["inflight_all_mean"] = p["inflight_all_sum"] / p["steps"] if p["steps"] else 0.0
    s = {"static_instructions": 0, "static_bytes": 0, "executed": 0, "covered_instructions": 0,
         "hot50": 0, "hot90": 0, "hot99": 0, "classes": {c: 0 for c in CLASSES},
         "executed_classes": {c: 0 for c in CLASSES}, "ref_static": {}, "ref_executed": {},
         "jmp_trigger_field": {}}
    for st in static:
        for k in ("static_instructions", "static_bytes", "executed", "covered_instructions",
                  "hot50", "hot90", "hot99"):
            s[k] += st.get(k, 0)
        for c in CLASSES:
            s["classes"][c] += st["classes"].get(c, 0)
            s["executed_classes"][c] += st.get("executed_classes", {}).get(c, 0)
        add_hist(s["ref_static"], st["ref_static"])
        add_hist(s["ref_executed"], st.get("ref_executed", {}))
        add_hist(s["jmp_trigger_field"], st["jmp_trigger_field"])
    s["coverage"] = s["covered_instructions"] / s["static_instructions"] if s["static_instructions"] else 0.0
    return p, s


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("metrics")
    ap.add_argument("--rv32", help="rv32_metrics.py JSON for the same benchmarks")
    ap.add_argument("--whole", action="store_true", help="whole run instead of the benchmark window")
    args = ap.parse_args()
    with open(args.metrics) as f:
        data = json.load(f)
    rv = {}
    if args.rv32:
        with open(args.rv32) as f:
            rv = json.load(f)
    key = "whole" if args.whole else "window"
    names = sorted(data)
    if any("static" not in data[n] for n in names):
        sys.exit("metrics file has no static view; rerun run_embench.py --metrics --profile")
    rows = [(n, data[n][key], data[n]["static"], rv.get(n)) for n in names]
    tot_w, tot_s = pooled([r[1] for r in rows], [r[2] for r in rows])
    tot_rv = None
    if rv and all(r[3] for r in rows):
        tot_rv = {k: sum(r[3][k] for r in rows) for k in
                  ("window_instructions", "window_bytes", "static_bytes", "static_instructions",
                   "window_control", "window_loads", "window_stores")}
    all_rows = rows + [("**pooled**", tot_w, tot_s, tot_rv)]
    out = []

    # 1. Code size
    out.append("### Code size\n")
    hdr = ["benchmark", "Scry text bytes", "Scry instrs", "rv32imac text bytes", "ratio",
           "Scry executed bytes", "rv32imac executed bytes", "ratio"]
    t = []
    for n, w, s, r in rows:
        xb = 2 * w["instructions"]
        t.append([n, f"{s['static_bytes']:,}", f"{s['static_instructions']:,}",
                  f"{r['static_bytes']:,}" if r else "-", ratio(s["static_bytes"], r["static_bytes"]) if r else "-",
                  f"{xb:,}", f"{r['window_bytes']:,}" if r else "-",
                  ratio(xb, r["window_bytes"]) if r else "-"])
    # This table compares the two ISAs, so its summary row is the geometric
    # mean of every column (the ratio cells are then also the ratio of the
    # mean counts); the pooled sums follow in a sentence.
    gm = [gmean([r[2]["static_bytes"] for r in rows]), gmean([r[2]["static_instructions"] for r in rows])]
    if tot_rv:
        gm += [gmean([r[3]["static_bytes"] for r in rows]),
               gmean([r[2]["static_bytes"] / r[3]["static_bytes"] for r in rows])]
    else:
        gm += [None, None]
    gm.append(gmean([2 * r[1]["instructions"] for r in rows]))
    if tot_rv:
        gm += [gmean([r[3]["window_bytes"] for r in rows]),
               gmean([2 * r[1]["instructions"] / r[3]["window_bytes"] for r in rows])]
    else:
        gm += [None, None]
    t.append(["**geometric mean**"] + [("-" if g is None else f"{g:.2f}" if i in (3, 6) else f"{round(g):,}")
                                      for i, g in enumerate(gm)])
    out.append(table(hdr, t))
    xb = 2 * tot_w["instructions"]
    if tot_rv:
        out.append(f"\nPooled over the benchmarks: {tot_s['static_bytes']:,} against "
                   f"{tot_rv['static_bytes']:,} bytes of code (ratio "
                   f"{ratio(tot_s['static_bytes'], tot_rv['static_bytes'])}), "
                   f"{tot_s['static_instructions']:,} against {tot_rv['static_instructions']:,} static "
                   f"instructions ({ratio(tot_s['static_instructions'], tot_rv['static_instructions'])}), "
                   f"{xb:,} against {tot_rv['window_bytes']:,} executed bytes "
                   f"({ratio(xb, tot_rv['window_bytes'])}).")
    else:
        out.append(f"\nPooled over the benchmarks: {tot_s['static_bytes']:,} bytes of code, "
                   f"{tot_s['static_instructions']:,} static instructions, {xb:,} executed bytes.")

    # 2. Dynamic mix
    out.append("\n### Dynamic instruction mix\n")
    hdr = ["benchmark", "executed", "control", "dataflow", "alu", "cast/pick", "const/grow",
           "memory", "frame", "stack", "rv32 control", "rv32 ld/st"]
    t = []
    for n, w, s, r in all_rows:
        xc = s["executed_classes"]
        e = s["executed"]
        t.append([n, f"{w['instructions']:,}", pct(xc["control"], e), pct(xc["dataflow"], e),
                  pct(xc["alu"], e), pct(xc["cast"] + xc["pick"], e), pct(xc["const"], e),
                  pct(xc["memory"], e), pct(xc["frame"], e), pct(xc["stack"], e),
                  pct(r["window_control"], r["window_instructions"]) if r else "-",
                  pct(r["window_loads"] + r["window_stores"], r["window_instructions"]) if r else "-"])
    out.append(table(hdr, t))

    # 3. Static mix
    out.append("\n### Static instruction mix\n")
    hdr = ["benchmark", "static instrs", "control", "dataflow", "alu", "cast/pick", "const/grow",
           "memory", "frame", "stack", "executed once", "for 90% of execution"]
    t = []
    for n, w, s, r in all_rows:
        c = s["classes"]
        k = s["static_instructions"]
        t.append([n, f"{k:,}", pct(c["control"], k), pct(c["dataflow"], k), pct(c["alu"], k),
                  pct(c["cast"] + c["pick"], k), pct(c["const"], k), pct(c["memory"], k),
                  pct(c["frame"], k), pct(c["stack"], k), pct(s["covered_instructions"], k),
                  f"{s['hot90']:,} ({pct(s['hot90'], k)})"])
    out.append(table(hdr, t))

    # 4. Operand lifetime
    out.append("\n### Operand lifetime\n")
    hdr = ["benchmark", "operands", "per instr", "mean", "median", "p90", "at 0", "≥ 32",
           "ref mean", "ref at 0", "discarded", "forwards per instr"]
    t = []
    for n, w, s, r in all_rows:
        v = w["value_ref"]
        rx = s["ref_executed"]
        t.append([n, f"{w['produced']:,}", f"{w['produced'] / w['instructions']:.2f}",
                  f"{h_mean(v):.1f}", h_quantile(v, 0.5), h_quantile(v, 0.9),
                  f"{h_share(v, lambda d: d == 0):.0f}%", f"{h_share(v, lambda d: d >= 32):.1f}%",
                  f"{h_mean(rx):.1f}", f"{h_share(rx, lambda d: d == 0):.0f}%",
                  pct(w["discards"], w["produced"]), f"{w['forwards'] / w['instructions']:.2f}"])
    out.append(table(hdr, t))

    # 5. Control flow
    out.append("\n### Control-flow lifetime\n")
    hdr = ["benchmark", "jmp per 1000", "taken", "overridden", "trigger field mean",
           "issue→trigger mean", "at 0", "≥ 32", "calls per 1000",
           "call/ret trigger mean", "max depth"]
    t = []
    for n, w, s, r in all_rows:
        jd = w["jmp_dynamic"]
        jm = w["opcodes"].get("jmp", 0)
        cr = {}
        add_hist(cr, w["call_dynamic"])
        add_hist(cr, w["ret_dynamic"])
        t.append([n, per_k(jm, w["instructions"]), pct(w["jmp_taken"], jm), pct(w["jmp_overridden"], jm),
                  f"{h_mean(w['jmp_trigger']):.1f}", f"{h_mean(jd):.1f}",
                  f"{h_share(jd, lambda d: d == 0):.0f}%", f"{h_share(jd, lambda d: d >= 32):.0f}%",
                  per_k(w["opcodes"].get("call", 0), w["instructions"]), f"{h_mean(cr):.2f}",
                  w["call_depth_max"]])
    out.append(table(hdr, t))
    out.append("\n#### Jump distance as written (trigger field)\n")
    out.append(table(["benchmark"] + QUANTILE_HDR,
                     [[n] + quantile_row(w["jmp_trigger"]) for n, w, s, r in all_rows]))
    out.append("\n#### Jump distance as executed (issue to trigger)\n")
    out.append(table(["benchmark"] + QUANTILE_HDR,
                     [[n] + quantile_row(w["jmp_dynamic"]) for n, w, s, r in all_rows]))

    # 6. Memory
    out.append("\n### Memory traffic\n")
    hdr = ["benchmark", "loads per 1000", "bytes per load", "from frame slots", "stores per 1000",
           "bytes per store", "to frame slots", "load→use mean", "rv32 loads per 1000",
           "rv32 stores per 1000"]
    t = []
    for n, w, s, r in all_rows:
        ld = w["load_dynamic"]
        t.append([n, per_k(w["loads"], w["instructions"]),
                  f"{w['load_bytes'] / w['loads']:.1f}" if w["loads"] else "-",
                  pct(w["stack_loads"], w["loads"]), per_k(w["stores"], w["instructions"]),
                  f"{w['store_bytes'] / w['stores']:.1f}" if w["stores"] else "-",
                  pct(w["stack_stores"], w["stores"]), f"{h_mean(ld):.1f}",
                  per_k(r["window_loads"], r["window_instructions"]) if r else "-",
                  per_k(r["window_stores"], r["window_instructions"]) if r else "-"])
    out.append(table(hdr, t))
    out.append("\n#### Load distance as written (output reference)\n")
    out.append(table(["benchmark"] + QUANTILE_HDR,
                     [[n] + quantile_row(w["load_ref"]) for n, w, s, r in all_rows]))
    out.append("\n#### Load distance as executed (load to use)\n")
    out.append(table(["benchmark"] + QUANTILE_HDR,
                     [[n] + quantile_row(w["load_dynamic"]) for n, w, s, r in all_rows]))

    # 7. In flight
    out.append("\n### Operands in flight\n")
    hdr = ["benchmark", "frame mean", "frame max", "all mean", "all max"]
    t = [[n, f"{w['inflight_frame_mean']:.1f}", w["inflight_frame_max"],
          f"{w['inflight_all_mean']:.1f}", w["inflight_all_max"]] for n, w, s, r in all_rows]
    out.append(table(hdr, t))

    print("\n".join(out))


if __name__ == "__main__":
    main()
