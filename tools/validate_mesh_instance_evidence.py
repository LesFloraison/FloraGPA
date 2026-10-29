"""Validate saved instance-storage observations and the unchanged legacy UI test."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile

from validate_native_mesh_captures import ADDITIONAL_CASES, load, require, validate_stage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--records", type=Path, required=True)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--baseline-ui-log", type=Path, required=True)
    parser.add_argument("--current-ui-log", type=Path, required=True)
    parser.add_argument("--comparison", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    manifest = load(args.records / "manifest.json")
    require(manifest["completed"] is True and len(manifest["cases"]) == 10, "complete report collection")
    require(sum(c["origin"] == "new_frozen_main_job" for c in manifest["cases"]) == 3 and
            sum(c["origin"] == "retained_baseline" for c in manifest["cases"]) == 7, "baseline provenance matrix")
    identities, files = set(), 0
    for case in manifest["cases"]:
        require(case["passed"] is True, "baseline case completion")
        result = load(args.records / "reports" / case["case"] / "result.json")
        job = load(args.records / "reports" / case["case"] / "job.json")
        require(job["gpa_event"] == result["gpa_event"] == case["gpa_event"] and
                job["instance"] == case["instance"] and job["stage"] == result["stage"], "baseline job selection")
        identity = (case["capture"]["sha256"], result["gpa_event"], case["instance"], result["stage"])
        require(identity not in identities, "distinct baseline selections")
        identities.add(identity)
        if case["origin"] == "new_frozen_main_job":
            require(case["exit_code"] == 0 and len(case["command"]) == 3, "completed frozen-main job")
        for entry in case["files"]:
            path = Path(entry["path"])
            require(path.parent.resolve() == (args.records / "reports" / case["case"]).resolve(),
                    "record file location")
            raw = path.read_bytes()
            require(len(raw) == entry["bytes"] and hashlib.sha256(raw).hexdigest() == entry["sha256"],
                    "record file identity")
            require((args.records / "ui-reports" / (case["case"] + "-native") / path.name).read_bytes() == raw,
                    "UI fixture provenance")
            if case["origin"] == "retained_baseline":
                require((Path(case["source"]) / path.name).read_bytes() == raw, "unchanged retained baseline")
            files += 1
    for path in (args.baseline_ui_log, args.current_ui_log):
        text = path.read_text(encoding="utf-8")
        require(text.count("PASS   : ReplayMeshUiTests::recordedReportsAndExports()") == 1 and
                re.search(r"Totals: 3 passed, 0 failed, 0 skipped, 0 blacklisted,", text), "UI report regression")
    validation = load(args.native / "revalidation.json")
    expected = {f"{v}-{label}-{event}-{instance}" for v in (4318, 32902, 0)
                for label, event, instance in ADDITIONAL_CASES}
    require(validation["completed"] is True and validation["passed"] is True and
            {c["case"] for c in validation["cases"]} == expected and len(validation["cases"]) == 9,
            "native additional-instance matrix")
    require(all(c["passed"] and c["corruption_controls"] == 4 for c in validation["cases"]),
            "native cross-path comparisons")
    for vendor in (4318, 32902, 0):
        for label, event, instance in ADDITIONAL_CASES:
            for stage in ("vs-inputs", "final"):
                validate_stage(args.native / f"{vendor}-{label}-{event}-{instance}" / stage,
                               label, event, instance, vendor, validation["worker_sha256"], stage, True)
    comparison = load(args.comparison)
    require(comparison["completed"] is True and comparison["strict_exports_equal"] is False and
            comparison["referenced_records_equal"] is True and comparison["indices_equal"] is True,
            "strict comparison remains failing")
    require([c["case"] for c in comparison["cases"]] == [f"{a}-{b}-{c}" for a, b, c in ADDITIONAL_CASES],
            "comparison cases")
    for case, expected_vertices in zip(comparison["cases"], (2248, 1405, 834), strict=True):
        require(case["baseline_vertices"] == expected_vertices, "observed baseline vertex storage")
        if case["case"].startswith("bf1"):
            require(case["strict_exports_equal"] is False and case["native_vertices"] == 281 and
                    case["all_instances"]["from_selected_instance"]["vertices"] == expected_vertices and
                    case["all_instances"]["from_selected_instance"]["matches_baseline_buffer"] is True,
                    "native full storage explains baseline tail")
        else:
            require(case["strict_exports_equal"] is True and case["native_vertices"] == 834, "last-instance exports")
    # Corrupt copies only; original jobs and all recorded outputs remain unchanged.
    source = args.native / "4318-bf1-1415-1" / "vs-inputs"
    controls = []
    for target in ("missing_mapping", "channels", "missing_reason", "reason", "references", "compact_bytes"):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary) / "case"
            shutil.copytree(source, folder)
            path = folder / "presentation.json"
            presentation = load(path)
            mapping = presentation["replay_mesh"]
            if target == "missing_mapping":
                del presentation["replay_mesh"]
            elif target == "channels":
                mapping["position_components"] = [0, 2, 1, 3]
            elif target == "missing_reason":
                del mapping["obj_unavailable_reason"]
            elif target == "reason":
                mapping["obj_unavailable_reason"] = "Invented"
            elif target == "references":
                mapping["reference_vertices"][0] += 1
            else:
                binary = folder / "post_vertices.bin"
                raw = binary.read_bytes()
                binary.write_bytes(bytes([raw[0] ^ 1]) + raw[1:])
            path.write_text(json.dumps(presentation), encoding="utf-8")
            try:
                validate_stage(folder, "bf1", 1415, 1, 4318, validation["worker_sha256"], "vs-inputs", True)
            except ValueError as error:
                require(str(error) == ("missing native semantic position mapping" if target == "missing_mapping" else
                                       "compact vertex records" if target == "compact_bytes" else
                                       "independent compact identity mapping"), "specific corruption rejection")
                controls.append({"target": target, "error": str(error)})
            else:
                raise ValueError("Accepted corrupted presentation: " + target)
    report = {"schema": 1, "status": "observations_verified", "baseline_selections": len(identities),
              "baseline_files": files, "native_cases": 9, "native_jobs": 18, "native_corruptions_rejected": 36,
              "metadata_corruptions_rejected": controls, "strict_exports_equal": False,
              "full_mesh_parity": False, "full_renderdoc_removal": False}
    with args.out.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
