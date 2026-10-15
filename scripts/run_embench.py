#!/usr/bin/env python3
"""Build the Embench-IoT benchmarks with the Scry toolchain and run them on
the scryer simulator.

Each benchmark is compiled from its sources plus the Embench support files
and the Scry board support in embench/config/scry, then executed with
"scryer --machine-mode".  A benchmark passes when the exit code is 0, i.e.
verify_benchmark() accepted the result.  With --metrics each benchmark is
also run under scryer-metrics (tools/scryer), which reports, for the window
between start_trigger and stop_trigger: executed instructions, non-dataflow
instructions, the dataflow share, the mean jmp issue-to-trigger distance, the
mean ld output reference and the in-flight operand count (mean/max).

Usage:
    run_embench.py [--embench <dir>] [--clang <clang>] [--scryer <scryer>]
                   [-O 2] [--gsf 1] [--timeout 600] [--metrics] [--json out.json]
                   [-j N] [benchmarks...]

The Embench sources are cloned from https://github.com/embench/embench-iot
into third_party/embench-iot when --embench is not given.
"""
import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
TRIPLE = "scry32-unknown-none-elf"
EXE = ".exe" if os.name == "nt" else ""
EMBENCH_REPO = "https://github.com/embench/embench-iot.git"
EMBENCH_REV = "09c2ed8c3b7008c95d08b038de4a3f6dc103ed70"

# Benchmarks that cannot run on the stock scryer simulator and why.  They are
# still built and run, but a failure is reported as XFAIL and does not fail
# the script (pass --strict to count them).
XFAIL = {
    "huffbench": "needs a 2 KB frame plus callers; scryer's stack is fixed at 4 KB",
    "wikisort": "needs 4 KB frames; scryer's stack is fixed at 4 KB",
}


def hist_mean(h):
    """Mean of a {value: count} histogram."""
    n = sum(h.values())
    return sum(int(k) * v for k, v in h.items()) / n if n else 0.0


def find_default(candidates):
    for c in candidates:
        if c.exists():
            return c
    return None


def warn_if_runtime_stale(clang):
    """The runtime is compiled by the Scry clang; a clang newer than the
    installed crt0.o means the runtime (and every link) carries old code."""
    crt0 = clang.parent.parent / "lib" / "clang-runtimes" / TRIPLE / "lib" / "crt0.o"
    if crt0.exists() and crt0.stat().st_mtime < clang.resolve().stat().st_mtime:
        print(f"warning: {crt0} is older than {clang}; rerun scripts/build_runtime.py",
              file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--embench", default=str(ROOT / "third_party" / "embench-iot"))
    ap.add_argument("--clang")
    ap.add_argument("--scryer")
    ap.add_argument("-O", "--opt", default="2")
    ap.add_argument("--gsf", default="1", help="global scale factor")
    ap.add_argument("--warmup", default="1", help="warm-up heat")
    ap.add_argument("--timeout", type=int, default=1800, help="seconds per run")
    ap.add_argument("--metrics", action="store_true",
                    help="also run under scryer-metrics (tools/scryer) and print the code "
                         "metrics of the benchmark() window")
    ap.add_argument("--metrics-bin", help="path to scryer-metrics (default: next to scryer)")
    ap.add_argument("--json", help="write all per-benchmark metrics to this JSON file")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--cflags", action="append", default=[],
                    help="extra compiler flag (repeatable; use --cflags=-mllvm --cflags=-x)")
    ap.add_argument("-j", "--jobs", type=int, default=1,
                    help="benchmarks to build and run in parallel (default 1)")
    ap.add_argument("--strict", action="store_true",
                    help="count the known simulator-limited benchmarks (XFAIL) as failures")
    ap.add_argument("benchmarks", nargs="*")
    args = ap.parse_args()

    clang = pathlib.Path(args.clang) if args.clang else find_default([
        ROOT / "install" / "bin" / ("clang" + EXE),
        ROOT / "build" / "llvm" / "bin" / ("clang" + EXE)])
    scryer = pathlib.Path(args.scryer) if args.scryer else find_default([
        ROOT / "build" / "scryer" / "release" / ("scryer" + EXE),
        ROOT / "install" / "bin" / ("scryer" + EXE)])
    if not clang or not clang.exists():
        sys.exit("clang not found; pass --clang")
    if not scryer or not scryer.exists():
        sys.exit("scryer not found; pass --scryer")
    warn_if_runtime_stale(clang)

    metrics_bin = None
    if args.metrics:
        metrics_bin = pathlib.Path(args.metrics_bin) if args.metrics_bin else find_default([
            ROOT / "build" / "scryer-metrics" / "release" / ("scryer-metrics" + EXE),
            ROOT / "install" / "bin" / ("scryer-metrics" + EXE)])
        if not metrics_bin or not metrics_bin.exists():
            sys.exit("scryer-metrics not found; run scripts/build_scryer.py or pass --metrics-bin")

    embench = pathlib.Path(args.embench)
    if not (embench / "src").exists():
        embench.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", EMBENCH_REPO, str(embench)], check=True)
        subprocess.run(["git", "-C", str(embench), "checkout", EMBENCH_REV], check=True)

    config = ROOT / "embench" / "config" / "scry"
    support = embench / "support"
    outdir = ROOT / "build" / "embench"
    outdir.mkdir(parents=True, exist_ok=True)

    names = args.benchmarks or sorted(p.name for p in (embench / "src").iterdir() if p.is_dir())
    results = []
    all_metrics = {}
    if args.metrics:
        print(f"{'':14} {'benchmark':18} {'instr':>11} {'non-DF':>11} {'DF%':>5} "
              f"{'jmp':>5} {'ld':>5} {'flight':>7}")
    def run_one(name):
        """Compile and run one benchmark; returns (status, size, instrs, metrics, lines)."""
        srcdir = embench / "src" / name
        sources = sorted(srcdir.glob("*.c")) + [support / "main.c", support / "beebsc.c",
                                                config / "boardsupport.c"]
        elf = outdir / (name + ".elf")
        cmd = [str(clang), "--target=" + TRIPLE, "-O" + args.opt, "-g0",
               "-I", str(support), "-I", str(config), "-I", str(srcdir),
               f"-DWARMUP_HEAT={args.warmup}", f"-DGLOBAL_SCALE_FACTOR={args.gsf}",
               "-DHAVE_BOARDSUPPORT_H", "-ffunction-sections",
               "-fdata-sections", "-Wl,--gc-sections", *args.cflags,
               *[str(s) for s in sources], "-o", str(elf), "-lm"]
        t0 = time.time()
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            return ("compile-error", 0, 0, None,
                    [f"FAIL {name}: compile error\n{r.stderr[-3000:]}"])
        size = elf.stat().st_size
        run = subprocess.run([str(scryer), str(elf), "--target=" + TRIPLE,
                              "--machine-mode", f"--timeout={args.timeout}"],
                             capture_output=True, text=True)
        elapsed = time.time() - t0
        status = "PASS" if run.returncode == 0 else f"FAIL(exit {run.returncode})"
        if run.returncode != 0 and name in XFAIL and not args.strict:
            status = f"XFAIL(exit {run.returncode})"
        instrs = 0
        metrics = None
        metrics_text = ""
        if args.metrics and run.returncode == 0:
            jpath = outdir / (name + ".metrics.json")
            subprocess.run([str(metrics_bin), str(elf), "--target=" + TRIPLE,
                            f"--timeout={args.timeout}", "--window-start", "start_trigger",
                            "--window-stop", "stop_trigger", "--json", str(jpath)],
                           capture_output=True, text=True)
            if jpath.exists():
                with open(jpath) as f:
                    metrics = json.load(f)
                w = metrics["window"]
                instrs = w["instructions"]
                metrics_text = (f" {instrs:11,d} {instrs - w['dataflow']:11,d} "
                                f"{100.0 * w['dataflow'] / max(instrs, 1):5.1f} "
                                f"{hist_mean(w['jmp_trigger']):5.1f} {hist_mean(w['load_ref']):5.1f} "
                                f"{w['inflight_frame_mean']:4.1f}/{w['inflight_frame_max']:<3d}")
                if not args.keep:
                    jpath.unlink()
        lines = []
        if args.metrics:
            lines.append(f"{status:14} {name:18}{metrics_text}")
        else:
            lines.append(f"{status:14} {name:18} elf={size:8d}B  {elapsed:6.1f}s")
        if status.startswith("XFAIL"):
            lines.append(f"               (expected: {XFAIL[name]})")
        elif run.returncode != 0 and run.stderr.strip():
            lines.append(run.stderr[-2000:])
        if not args.keep and elf.exists():
            elf.unlink()
        return (status, size, instrs, metrics, lines)

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = [(name, pool.submit(run_one, name)) for name in names]
        for name, fut in futures:
            status, size, instrs, metrics, lines = fut.result()
            print("\n".join(lines), flush=True)
            if metrics is not None:
                all_metrics[name] = metrics
            results.append((name, status, size, instrs))

    if args.json:
        with open(args.json, "w") as f:
            json.dump(all_metrics, f, indent=1, sort_keys=True)
        print(f"metrics written to {args.json}")
    if args.metrics and all_metrics:
        tot = sum(m["window"]["instructions"] for m in all_metrics.values())
        df = sum(m["window"]["dataflow"] for m in all_metrics.values())
        print(f"{'':14} {'total':18} {tot:11,d} {tot - df:11,d} {100.0 * df / max(tot, 1):5.1f}")

    passed = sum(1 for r in results if r[1] == "PASS")
    xfailed = sum(1 for r in results if r[1].startswith("XFAIL"))
    print(f"\n{passed}/{len(results)} benchmarks passed"
          + (f" ({xfailed} expected failures)" if xfailed else ""))
    return 0 if passed + xfailed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
