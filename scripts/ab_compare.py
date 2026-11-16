#!/usr/bin/env python3
"""Compares two builds of exchange-bench on the same benchmark.

Usage: scripts/ab_compare.py [--rounds N] BASELINE CANDIDATE -- BENCH_ARGS...

Example:
    scripts/ab_compare.py build/ab/bench-baseline build/release/bench/exchange-bench \\
        -- e2e --server pipeline --requests 50000 --runs 3

The two binaries are run alternately (baseline, candidate, baseline, ...) for
N rounds, so that a change in the machine's speed during the comparison
affects both equally. Each invocation writes its JSON report; the medians of
all invocations are then compared.
"""

import argparse
import json
import os
import shlex
import statistics
import subprocess
import sys
import tempfile


def median_results(path):
    """Returns {label: {statistic: value}} from one JSON report."""
    with open(path) as report_file:
        report = json.load(report_file)
    if report["benchmark"] == "e2e":
        return {
            server["server"]: {
                "p50": server["median"]["latency_ns"]["p50"],
                "p99": server["median"]["latency_ns"]["p99"],
                "mean": server["median"]["latency_ns"]["mean"],
                "per_second": server["median"]["requests_per_second"],
            }
            for server in report["servers"]
        }
    if report["benchmark"] == "engine":
        median = report["median"]
        return {
            "engine": {
                "p50": median["latency_ns"]["p50"],
                "p99": median["latency_ns"]["p99"],
                "mean": median["latency_ns"]["mean"],
                "per_second": median["commands_per_second"],
            }
        }
    if report["benchmark"] == "queue":
        return {"queue": {"per_second": report["median"]["spsc_messages_per_second"]}}
    raise SystemExit(f"unknown benchmark {report['benchmark']}")


def run(binary, bench_args, json_path):
    subprocess.run(
        [binary, *bench_args, "--json", json_path],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return median_results(json_path)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--baseline-args", default="", help="extra arguments for the baseline only")
    parser.add_argument(
        "--candidate-args", default="", help="extra arguments for the candidate only"
    )
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    parser.add_argument("bench_args", nargs=argparse.REMAINDER)
    arguments = parser.parse_args()
    bench_args = [a for a in arguments.bench_args if a != "--"]
    if not bench_args:
        parser.error("missing benchmark arguments after --")

    samples = {"baseline": {}, "candidate": {}}
    with tempfile.TemporaryDirectory() as directory:
        json_path = os.path.join(directory, "report.json")
        for round_number in range(1, arguments.rounds + 1):
            for name in ("baseline", "candidate"):
                extra = shlex.split(getattr(arguments, f"{name}_args"))
                results = run(getattr(arguments, name), bench_args + extra, json_path)
                for label, statistics_of_run in results.items():
                    for statistic, value in statistics_of_run.items():
                        samples[name].setdefault((label, statistic), []).append(value)
            print(f"round {round_number}/{arguments.rounds} done", file=sys.stderr)

    print(f"{arguments.rounds} rounds, baseline and candidate run alternately")
    print(f"arguments: {' '.join(bench_args)}")
    for name in ("baseline", "candidate"):
        if getattr(arguments, f"{name}_args"):
            print(f"{name} only: {getattr(arguments, f'{name}_args')}")
    header = "median  (min .. max)"
    print(f"{'':30} {'baseline':>28}   {'candidate':>28}   change")
    print(f"{'':30} {header:>28}   {header:>28}")
    for key in samples["baseline"]:
        if key not in samples["candidate"]:
            continue
        label, statistic = key
        # Latencies are reported in nanoseconds: shown as such for the engine,
        # in microseconds for requests over the network.
        if statistic == "per_second":
            scale, unit = 1.0, "/s"
        elif label == "engine":
            scale, unit = 1.0, "ns"
        else:
            scale, unit = 0.001, "us"
        columns = []
        medians = []
        for name in ("baseline", "candidate"):
            values = [value * scale for value in samples[name][key]]
            medians.append(statistics.median(values))
            columns.append(f"{medians[-1]:.1f} ({min(values):.1f} .. {max(values):.1f})")
        change = (medians[1] - medians[0]) / medians[0] * 100.0 if medians[0] else 0.0
        row = f"{label} {statistic} {unit}"
        print(f"{row:30} {columns[0]:>28}   {columns[1]:>28}   {change:+6.1f}%")


if __name__ == "__main__":
    main()
