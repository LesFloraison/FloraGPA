"""Record native texture observations without claiming texture instruction execution."""

import argparse
import json
from pathlib import Path
import re

from summarize_dxbc_execution import identity


def read_run(path):
    text = path.read_text(encoding="utf-8-sig")
    totals = re.search(r"Totals: (\d+) passed, (\d+) failed, (\d+) skipped", text)
    if totals is None:
        raise ValueError(f"Missing completed test totals: {path}")
    probes = []
    for line in text.splitlines():
        if not line.startswith("QINFO") or " {" not in line:
            continue
        item = json.loads(line[line.index(" {") + 1:])
        if item.get("probe") != "dxbc-texture-inputs":
            continue
        case = re.search(r"DxbcTextureInputTests::([^ ]+)", line).group(1)
        item["case"] = case
        item["test_passed"] = bool(re.search(
            r"^PASS\s+: DxbcTextureInputTests::" + re.escape(case) + r"$", text, re.MULTILINE))
        probes.append(item)
    return {**identity(path), "passed": int(totals[1]), "failed": int(totals[2]),
            "skipped": int(totals[3]),
            "failures": re.findall(r"^FAIL!\s+: (.*)$", text, re.MULTILINE),
            "probes": probes}


def observation(path):
    report = json.loads(path.read_text(encoding="utf-8-sig"))
    if report.get("schema") != 2 or report.get("shader_execution_performed") is not False:
        raise ValueError(f"Unexpected observation scope: {path}")
    result = {**identity(path), **{key: report.get(key) for key in (
        "observed", "error", "capture", "frame_sha256", "event", "stage", "shader",
        "shader_sha256", "source", "adapter_vendor", "adapter_device", "adapter_luid", "elapsed_ms")},
        "shader_execution_performed": False, "textures": [],
        "texture_views": report.get("texture_views", []),
        "unavailable": report.get("unavailable", [])}
    for texture in report.get("textures", []):
        result["textures"].append({key: texture[key] for key in (
            "resource", "type", "descriptor", "min_lod", "bytes", "sha256", "validity_sha256", "known_bytes")})
        result["textures"][-1]["subresource_count"] = len(texture["subresources"])
    return result


def passed(run):
    return not run["failed"] and not run["skipped"] and all(
        probe["passed"] and probe["test_passed"] for probe in run["probes"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, action="append", required=True)
    parser.add_argument("--regression", type=Path, action="append", default=[])
    parser.add_argument("--observation", type=Path, action="append", default=[])
    parser.add_argument("--source", type=Path, action="append", default=[])
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    runs = [read_run(path) for path in args.log]
    regressions = [read_run(path) for path in args.regression]
    observations = [observation(path) for path in args.observation]
    success = passed(runs[-1]) and all(map(passed, regressions)) and all(
        item["observed"] for item in observations)
    result = {"schema": 1, "scope": "native_dxbc_texture_input_research",
              "complete_release_acceptance": False, "latest_run_passed": passed(runs[-1]),
              "regressions_passed": all(map(passed, regressions)),
              "sources": [identity(path) for path in args.source], "runs": runs,
              "regressions": regressions, "observations": observations}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {len(runs)} runs and {len(observations)} observations; validation passed: {success}")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
