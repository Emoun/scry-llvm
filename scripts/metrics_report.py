#!/usr/bin/env python3
"""Markdown tables of the ISA and compiler metrics of an Embench run.

    metrics_report.py metrics.json [--rv32 rv32.json] [--whole]

metrics.json is written by run_embench.py --metrics --profile --json (the
static view under "static" needs --profile for the execution-weighted
figures); rv32.json by rv32_metrics.py --json for the same benchmarks.
Every per-benchmark table ends in two summary rows: **pooled**, computed
from the summed counts and histograms (means, shares and quantiles over
all benchmarks' events), and **geometric mean**, the geometric mean over
the benchmarks of each column's values (so a ratio there is also the
ratio of the mean counts).  A geometric mean leaves out benchmarks whose
value is zero (a zero would make the mean zero) and is then marked with
a dagger.

Tables: code size, dynamic instruction mix, static instruction mix,
constant groups (what the immediates built by const/grow are used for,
executed and in the code), ALU operands (what produced the values the
ALU instructions consume, per benchmark and per opcode), operand
lifetime, control-flow lifetime, jump distance quantiles (trigger
field as written and issue-to-trigger as executed), memory traffic, load
distance quantiles (output reference as written and load-to-use as
executed), operands in flight.
"""
import argparse
import json
import math
import sys

CLASSES = ("control", "dataflow", "alu", "cast", "pick", "const", "memory", "frame", "stack", "trap")
ALU_OPS = ("add", "sub", "mul", "div", "sh", "and", "or", "xor", "eq", "lt", "gt", "isnar", "narto")


def add_counts(acc, h):
    """Merge a {name: count} dict into acc."""
    for k, v in h.items():
        acc[k] = acc.get(k, 0) + v


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


class Num:
    """A table cell that keeps its value(s) for the geometric-mean row.

    fmt is a format string with one field per value; gfmt formats the
    geometric mean (by default fmt, with one decimal for integer formats
    when the mean is small).
    """

    def __init__(self, value, fmt, gfmt=None):
        self.values = value if isinstance(value, tuple) else (value,)
        self.fmt = fmt
        self.gfmt = gfmt

    def __str__(self):
        return self.fmt.format(*self.values)

    def format_mean(self, means):
        fmt = self.gfmt or self.fmt
        if self.gfmt is None:
            for spec in (":,.0f", ":.0f"):
                if spec in fmt and max(means) < 1000:
                    fmt = fmt.replace(spec, ":.1f")
        return fmt.format(*means)


def cnt(x):
    return Num(x, "{:,.0f}")


def num(x, decimals=1):
    return Num(x, "{:.%df}" % decimals)


def quantile_row(h):
    """mean, p10, p25, median, p75, p90, p99, max of a {value: count} histogram."""
    if not h_n(h):
        return ["-"] * len(QUANTILE_HDR)
    return [num(h_mean(h))] + [num(h_quantile(h, q), 0) for q in (0.1, 0.25, 0.5, 0.75, 0.9, 0.99)] + [num(h_max(h), 0)]


def h_share(h, pred):
    n = h_n(h)
    return 100.0 * sum(v for k, v in h.items() if pred(int(k))) / n if n else 0.0


def pct(a, b, decimals=1):
    return Num(100.0 * a / b, "{:.%df}%%" % decimals) if b else "-"


def per_k(a, b):
    return Num(1000.0 * a / b, "{:.1f}") if b else "-"


def ratio(a, b):
    return Num(a / b, "{:.2f}") if b else "-"


def gmean(xs):
    """Geometric mean of the positive values (a zero would make the whole mean zero)."""
    xs = [x for x in xs if x > 0]
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else 0.0


def table(header, rows, align=None):
    out = ["| " + " | ".join(header) + " |",
           "|" + "|".join(("---:" if i else "---") for i in range(len(header))) + "|"]
    for r in rows:
        out.append("| " + " | ".join(str(c) for c in r) + " |")
    return "\n".join(out)


def gmean_cell(cells):
    """Geometric mean of one column, formatted like its cells.

    "-" cells are left out, and so are zero values, which would otherwise
    make the mean zero; a cell computed without some benchmarks' values
    is marked with a dagger.
    """
    nums = [c for c in cells if isinstance(c, Num)]
    if not nums:
        return "-"
    k = len(nums[0].values)
    means = tuple(gmean(c.values[i] for c in nums) for i in range(k))
    partial = any(c.values[i] <= 0 for c in nums for i in range(k))
    return nums[0].format_mean(means) + ("†" if partial else "")


def quantile_table(named_hists):
    rows = [[n] + quantile_row(h) for n, h in named_hists]
    return emit(["benchmark"] + QUANTILE_HDR, rows[:-1], rows[-1])


def emit(header, rows, pooled_row):
    """A table of per-benchmark rows followed by the pooled and the geometric-mean row."""
    gm = ["**geometric mean**"] + [gmean_cell([r[i] for r in rows]) for i in range(1, len(header))]
    return table(header, rows + [pooled_row, gm])


def pooled(scopes, static):
    """One pseudo-benchmark whose counts and histograms are the sums."""
    p = {"instructions": 0, "dataflow": 0, "produced": 0, "discards": 0, "forwards": 0,
         "loads": 0, "load_bytes": 0, "stores": 0, "store_bytes": 0, "stack_loads": 0,
         "stack_load_bytes": 0, "stack_stores": 0, "stack_store_bytes": 0, "jmp_issued": 0,
         "jmp_taken": 0, "jmp_not_taken": 0, "jmp_overridden": 0, "call_depth_max": 0,
         "steps": 0, "inflight_frame_sum": 0.0,
         "inflight_all_sum": 0.0, "inflight_frame_max": 0, "inflight_all_max": 0,
         "opcodes": {}, "const_groups": 0, "const_consumer": {}, "const_direct": {},
         "const_length": {}, "alu_inputs": 0, "alu_input": {}, "alu_input_route": {},
         "alu_input_by_op": {}}
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
        p["const_groups"] += w.get("const_groups", 0)
        add_counts(p["const_consumer"], w.get("const_consumer", {}))
        add_counts(p["const_direct"], w.get("const_direct", {}))
        add_hist(p["const_length"], w.get("const_length", {}))
        p["alu_inputs"] += w.get("alu_inputs", 0)
        add_counts(p["alu_input"], w.get("alu_input", {}))
        add_counts(p["alu_input_route"], w.get("alu_input_route", {}))
        for op, c in w.get("alu_input_by_op", {}).items():
            add_counts(p["alu_input_by_op"].setdefault(op, {}), c)
    p["inflight_frame_mean"] = p["inflight_frame_sum"] / p["steps"] if p["steps"] else 0.0
    p["inflight_all_mean"] = p["inflight_all_sum"] / p["steps"] if p["steps"] else 0.0
    s = {"static_instructions": 0, "static_bytes": 0, "executed": 0, "covered_instructions": 0,
         "hot50": 0, "hot90": 0, "hot99": 0, "classes": {c: 0 for c in CLASSES},
         "executed_classes": {c: 0 for c in CLASSES}, "ref_static": {}, "ref_executed": {},
         "jmp_trigger_field": {},
         "const_groups": {"groups": 0, "instructions": 0, "by_class": {}}}
    for st in static:
        g = st.get("const_groups", {})
        s["const_groups"]["groups"] += g.get("groups", 0)
        s["const_groups"]["instructions"] += g.get("instructions", 0)
        add_counts(s["const_groups"]["by_class"], g.get("by_class", {}))
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
    for n, w, s, r in all_rows:
        xb = 2 * w["instructions"]
        t.append([n, cnt(s["static_bytes"]), cnt(s["static_instructions"]),
                  cnt(r["static_bytes"]) if r else "-", ratio(s["static_bytes"], r["static_bytes"]) if r else "-",
                  cnt(xb), cnt(r["window_bytes"]) if r else "-",
                  ratio(xb, r["window_bytes"]) if r else "-"])
    out.append(emit(hdr, t[:-1], t[-1]))

    # 2. Dynamic mix
    out.append("\n### Dynamic instruction mix\n")
    hdr = ["benchmark", "executed", "control", "dataflow", "alu", "cast/pick", "const/grow",
           "memory", "frame", "stack", "rv32 control", "rv32 ld/st", "control vs rv32", "ld/st vs rv32"]
    t = []
    for n, w, s, r in all_rows:
        xc = s["executed_classes"]
        e = s["executed"]
        ctl = sum(w["opcodes"].get(o, 0) for o in ("jmp", "call", "ret"))
        mem = w["loads"] + w["stores"]
        t.append([n, cnt(w["instructions"]), pct(xc["control"], e), pct(xc["dataflow"], e),
                  pct(xc["alu"], e), pct(xc["cast"] + xc["pick"], e), pct(xc["const"], e),
                  pct(xc["memory"], e), pct(xc["frame"], e), pct(xc["stack"], e),
                  pct(r["window_control"], r["window_instructions"]) if r else "-",
                  pct(r["window_loads"] + r["window_stores"], r["window_instructions"]) if r else "-",
                  ratio(ctl, r["window_control"]) if r else "-",
                  ratio(mem, r["window_loads"] + r["window_stores"]) if r else "-"])
    out.append(emit(hdr, t[:-1], t[-1]))
    out.append("\nThe two ratio columns compare absolute counts: Scry `jmp`, `call` and `ret` "
               "against rv32imac branches, jumps, calls and returns, and all Scry loads and "
               "stores, frame slots included, against rv32imac loads and stores.")

    # 3. Static mix
    out.append("\n### Static instruction mix\n")
    hdr = ["benchmark", "static instrs", "control", "dataflow", "alu", "cast/pick", "const/grow",
           "memory", "frame", "stack", "executed once", "for 90% of execution"]
    t = []
    for n, w, s, r in all_rows:
        c = s["classes"]
        k = s["static_instructions"]
        t.append([n, cnt(k), pct(c["control"], k), pct(c["dataflow"], k), pct(c["alu"], k),
                  pct(c["cast"] + c["pick"], k), pct(c["const"], k), pct(c["memory"], k),
                  pct(c["frame"], k), pct(c["stack"], k), pct(s["covered_instructions"], k),
                  Num((s["hot90"], 100.0 * s["hot90"] / k), "{:,.0f} ({:.1f}%)")])
    out.append(emit(hdr, t[:-1], t[-1]))

    # 3b. Constant groups
    if all("const_groups" in r[1] for r in rows):
        out.append("\n### Constant groups\n")
        hdr = ["benchmark", "groups", "per 1000 instrs", "instrs per group", "alu", "control",
               "ld/st", "to slot", "cast/pick", "discarded", "via echo/dup"]
        t = []
        for n, w, s, r in all_rows:
            g = w["const_groups"]
            c = w["const_consumer"]
            length = sum(int(k) * v for k, v in w["const_length"].items())
            t.append([n, cnt(g), per_k(g, w["instructions"]), num(length / g, 2) if g else "-",
                      pct(c.get("alu", 0), g), pct(c.get("control", 0), g), pct(c.get("memory", 0), g),
                      pct(c.get("frame", 0), g), pct(c.get("cast", 0) + c.get("pick", 0), g),
                      pct(c.get("discarded", 0), g), pct(w["const_direct"].get("dataflow", 0), g)])
        out.append(emit(hdr, t[:-1], t[-1]))
        out.append("\n#### Constant groups in the code\n")
        hdr = ["benchmark", "groups", "instrs", "of the code", "alu", "control", "ld/st", "to slot",
               "echo/dup", "other"]
        t = []
        for n, w, s, r in all_rows:
            g = s["const_groups"]
            k, c = g["groups"], g["by_class"]
            other = k - sum(c.get(x, 0) for x in ("alu", "control", "memory", "frame", "dataflow"))
            t.append([n, cnt(k), cnt(g["instructions"]), pct(g["instructions"], s["static_instructions"]),
                      pct(c.get("alu", 0), k), pct(c.get("control", 0), k), pct(c.get("memory", 0), k),
                      pct(c.get("frame", 0), k), pct(c.get("dataflow", 0), k), pct(other, k)])
        out.append(emit(hdr, t[:-1], t[-1]))

    # 3c. ALU operands
    if all("alu_input" in r[1] for r in rows):
        out.append("\n### ALU operands\n")
        origins = ("const", "alu", "load", "slot", "cast", "frame")

        def origin_cells(n, c):
            calls = c.get("argument", 0) + c.get("return", 0)
            other = n - sum(c.get(x, 0) for x in origins) - calls
            return [pct(c.get(x, 0), n) for x in origins] + [pct(calls, n), pct(other, n)]

        hdr = ["benchmark", "operands", "per ALU instr", "const", "alu", "load", "slot", "cast",
               "frame addr", "arg/ret", "other", "via echo", "dup copy", "via pick"]
        t = []
        for n, w, s, r in all_rows:
            k = w["alu_inputs"]
            alu_n = sum(v for o, v in w["opcodes"].items() if o in ALU_OPS)
            rt = w["alu_input_route"]
            t.append([n, cnt(k), num(k / alu_n, 2) if alu_n else "-"] + origin_cells(k, w["alu_input"])
                     + [pct(rt.get("echo", 0), k), pct(rt.get("dup", 0), k), pct(rt.get("pick", 0), k)])
        out.append(emit(hdr, t[:-1], t[-1]))
        out.append("\n#### ALU operands by opcode\n")
        hdr = ["opcode", "operands", "of all", "const", "alu", "load", "slot", "cast", "frame addr",
               "arg/ret", "other"]
        t = []
        by_op = tot_w["alu_input_by_op"]
        for op in sorted(by_op, key=lambda o: -sum(by_op[o].values())):
            k = sum(by_op[op].values())
            t.append([op, cnt(k), pct(k, tot_w["alu_inputs"])] + origin_cells(k, by_op[op]))
        out.append(table(hdr, t))

    # 4. Operand lifetime
    out.append("\n### Operand lifetime\n")
    hdr = ["benchmark", "operands", "per instr", "mean", "median", "p90", "at 0", "≥ 32",
           "ref mean", "ref at 0", "discarded", "forwards per instr"]
    t = []
    for n, w, s, r in all_rows:
        v = w["value_ref"]
        rx = s["ref_executed"]
        t.append([n, cnt(w["produced"]), num(w["produced"] / w["instructions"], 2),
                  num(h_mean(v)), num(h_quantile(v, 0.5), 0), num(h_quantile(v, 0.9), 0),
                  Num(h_share(v, lambda d: d == 0), "{:.0f}%"), Num(h_share(v, lambda d: d >= 32), "{:.1f}%"),
                  num(h_mean(rx)), Num(h_share(rx, lambda d: d == 0), "{:.0f}%"),
                  pct(w["discards"], w["produced"]), num(w["forwards"] / w["instructions"], 2)])
    out.append(emit(hdr, t[:-1], t[-1]))

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
                  num(h_mean(w["jmp_trigger"])), num(h_mean(jd)),
                  Num(h_share(jd, lambda d: d == 0), "{:.0f}%"), Num(h_share(jd, lambda d: d >= 32), "{:.0f}%"),
                  per_k(w["opcodes"].get("call", 0), w["instructions"]), num(h_mean(cr), 2),
                  num(w["call_depth_max"], 0)])
    out.append(emit(hdr, t[:-1], t[-1]))
    out.append("\n#### Jump distance as written (trigger field)\n")
    out.append(quantile_table([(n, w["jmp_trigger"]) for n, w, s, r in all_rows]))
    out.append("\n#### Jump distance as executed (issue to trigger)\n")
    out.append(quantile_table([(n, w["jmp_dynamic"]) for n, w, s, r in all_rows]))

    # 6. Memory
    out.append("\n### Memory traffic\n")
    hdr = ["benchmark", "loads per 1000", "bytes per load", "from frame slots", "stores per 1000",
           "bytes per store", "to frame slots", "load→use mean", "rv32 loads per 1000",
           "rv32 stores per 1000"]
    t = []
    for n, w, s, r in all_rows:
        ld = w["load_dynamic"]
        t.append([n, per_k(w["loads"], w["instructions"]),
                  num(w["load_bytes"] / w["loads"]) if w["loads"] else "-",
                  pct(w["stack_loads"], w["loads"]), per_k(w["stores"], w["instructions"]),
                  num(w["store_bytes"] / w["stores"]) if w["stores"] else "-",
                  pct(w["stack_stores"], w["stores"]), num(h_mean(ld)),
                  per_k(r["window_loads"], r["window_instructions"]) if r else "-",
                  per_k(r["window_stores"], r["window_instructions"]) if r else "-"])
    out.append(emit(hdr, t[:-1], t[-1]))
    out.append("\n#### Load distance as written (output reference)\n")
    out.append(quantile_table([(n, w["load_ref"]) for n, w, s, r in all_rows]))
    out.append("\n#### Load distance as executed (load to use)\n")
    out.append(quantile_table([(n, w["load_dynamic"]) for n, w, s, r in all_rows]))

    # 7. In flight
    out.append("\n### Operands in flight\n")
    hdr = ["benchmark", "frame mean", "frame max", "all mean", "all max"]
    t = [[n, num(w["inflight_frame_mean"]), num(w["inflight_frame_max"], 0),
          num(w["inflight_all_mean"]), num(w["inflight_all_max"], 0)] for n, w, s, r in all_rows]
    out.append(emit(hdr, t[:-1], t[-1]))

    print("\n".join(out))


if __name__ == "__main__":
    main()
