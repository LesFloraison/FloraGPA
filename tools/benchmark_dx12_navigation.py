"""Run real GUI navigation with isolated evidence and explicit replay policy.

The compatibility baseline disables resident replay, fast previews and full-frame
reuse. CPU metadata remains enabled, so compare content latency, not old GUI list
latency. The first sample is cold; later samples include the first controller open
on the prepared frame. Record that sample separately when interpreting warm data.
"""
import argparse
import json
import math
import os
from pathlib import Path
import statistics
import subprocess


def distribution(values):
    ordered = sorted(values)
    return {"count": len(ordered), "p50_ms": statistics.median(ordered),
            "p95_ms": ordered[max(0, math.ceil(len(ordered) * .95) - 1)]} if ordered else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tests", required=True, type=Path)
    parser.add_argument("--capture-dir", required=True, type=Path)
    parser.add_argument("--capture", required=True)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--executions", required=True, help="At least 20 comma-separated execution IDs")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--isolate-queries", action="store_true")
    parser.add_argument("--require-reuse", action="store_true", help="Fail if this supported sample cannot prepare a reusable frame")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    for key in ("FLORA_DISABLE_RDC_SERVICE", "FLORA_DISABLE_FAST_PREVIEW", "FLORA_DISABLE_FRAME_REUSE",
                "FLORA_RESOURCE_ISOLATE", "FLORA_REQUIRE_FRAME_REUSE"):
        env.pop(key, None)
    env.update(QT_QPA_PLATFORM="offscreen", QTEST_FUNCTION_TIMEOUT="1200000",
               FLORA_DX12_CAPTURE_DIR=str(args.capture_dir.resolve()), FLORA_RESOURCE_CAPTURE=args.capture,
               FLORA_RENDERDOC_LIBRARY=str(args.library.resolve()), FLORA_RESOURCE_EXECUTIONS=args.executions,
               FLORA_RESOURCE_BENCHMARK=str((args.out / "navigation.json").resolve()))
    qt = Path("D:/Qt/6.11.2/msvc2022_64/bin")
    if qt.exists():
        env["PATH"] = str(qt) + os.pathsep + env.get("PATH", "")
    if args.baseline:
        for key in ("FLORA_DISABLE_RDC_SERVICE", "FLORA_DISABLE_FAST_PREVIEW", "FLORA_DISABLE_FRAME_REUSE"):
            env[key] = "1"
    if args.isolate_queries:
        env["FLORA_RESOURCE_ISOLATE"] = "1"
    if args.require_reuse:
        env["FLORA_REQUIRE_FRAME_REUSE"] = "1"
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    result = subprocess.run([str(args.tests.resolve()), "resourceNavigationBenchmark", "-o",
                             str((args.out / "test.log").resolve()) + ",txt"], env=env,
                            creationflags=flags, timeout=1200)
    evidence = args.out / "navigation.json"
    report = json.loads(evidence.read_text(encoding="utf-8")) if evidence.exists() else {}
    rows = report.get("samples", [])
    summary = dict(capture=args.capture, compatibility_baseline=args.baseline,
                   query_policy="isolate_readback_outputs" if args.isolate_queries else "strict",
                   exit_code=result.returncode, cold=rows[0] if rows else None,
                   prepared_content=distribution([r["preview_ms"] for r in rows[1:] if r.get("content_ready")]),
                   prepared_lists=distribution([r["list_ms"] for r in rows[1:]]),
                   captures=report.get("captures"), full_frame_ready=report.get("full_frame_ready"))
    summary["preparation_state"] = report.get("preparation_state")
    summary["attempted"] = len(rows)
    summary["failed_samples"] = [r for r in rows if not r.get("content_ready")]
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2), flush=True)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
