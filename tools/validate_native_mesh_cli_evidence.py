"""Check retained CLI publication evidence, including raw outputs without a mesh presentation."""
import argparse
import csv
import io
import json
from pathlib import Path
import re
import shutil
import struct
import tempfile

from validate_native_mesh_cli import context, verify
from validate_native_mesh_captures import digest, load, require, selected_cases


def synthetic(folder, vendor, mode, worker_hash):
    job, result = context(folder, "dx11-native/11", worker_hash)
    require(digest((folder / "source_fixture.gpa_frame").read_bytes()) == job["frame_sha256"],
            "retained synthetic frame identity")
    require(job["action"] == "mesh" and job["options"]["gpa_event"] == 200 and
            job["options"]["instance"] == 0 and job["options"]["stage"] == ("hs" if mode == "hs" else "vs-inputs"),
            "synthetic CLI selection")
    require(result["context"]["adapter"]["vendor"] == (vendor or 5140) and
            result["context"]["adapter"]["warp"] == (vendor == 0), "synthetic adapter")
    available = mode in ("selected", "remaining")
    reasons = {"hs": "Hull phase output uses the hull inspection representation",
               "no-position": "No float SV_Position output; raw attributes remain available"}
    require(result["mesh_presentation"] == {"schema": 1, "available": available, "reason": reasons.get(mode)},
            "presentation availability")
    expected = [(float(i), 0., i // 3 + (i % 3) * .25 if mode == "hs" else 0., 1.) for i in range(6)]
    require(list(struct.iter_unpack("<4f", (folder / "vertices.bin").read_bytes())) == expected,
            "raw shader outputs")
    if mode == "hs":
        require(list(struct.iter_unpack("<I", (folder / "vertices.validity.bin").read_bytes())) == [(1,)] * 24,
                "control-point write validity")
        require(list(struct.iter_unpack("<I", (folder / "patch_constants.bin").read_bytes())) ==
                [(v,) for patch in range(2) for v in (0x3f800000, 42 + patch, 0x3f800000, 0x3f800000, 0x3f800000)],
                "patch constant raw bits")
        require(list(struct.iter_unpack("<I", (folder / "patch_constants.validity.bin").read_bytes())) == [(1,)] * 10,
                "patch constant write validity")
    if mode == "remaining":
        require(job["options"]["vertex_storage"] == "remaining", "requested storage")
        later = [(float(i), 1., 0., 1.) for i in range(6)]
        require(list(struct.iter_unpack("<4f", (folder / "remaining_vertices.bin").read_bytes())) == later,
                "later instance shader outputs")
        expected += later
    for name in ("post_vertices.bin", "post_vertices.csv", "post_geometry.obj", "presentation.json"):
        require((name in result["artifacts"]) == available and (folder / name).exists() == available,
                "published presentation file set")
    if available:
        require(list(struct.iter_unpack("<4f", (folder / "post_vertices.bin").read_bytes())) == expected,
                "published position bytes")
        rows = list(csv.reader(io.StringIO((folder / "post_vertices.csv").read_text(encoding="utf-8"))))
        require(rows[0] == ["vertex", "x", "y", "z", "w"] and len(rows) == len(expected) + 1, "CSV layout")
        for i, (row, value) in enumerate(zip(rows[1:], expected, strict=True)):
            require(int(row[0]) == i and tuple(map(float, row[1:])) == value, "CSV values")
        obj = [row.split() for row in (folder / "post_geometry.obj").read_text(encoding="utf-8").splitlines()]
        require([tuple(map(float, row[1:])) for row in obj if row and row[0] == "v"] == [v[:3] for v in expected],
                "OBJ positions")
        require([row[1:] for row in obj if row and row[0] == "f"] == [["1", "2", "3"], ["4", "5", "6"]],
                "selected OBJ faces")
        mapping = load(folder / "presentation.json")["replay_mesh"]
        require(mapping["vertex_sources"] == list(range(len(expected))) and
                mapping["reference_vertices"] == list(range(6)) and mapping["index_count"] == 6,
                "selected reference mapping")
    return {"case": f"{vendor}-{mode}", "available": available, "raw_vertices": 6,
            "presented_vertices": len(expected) if available else None, "context_id": result["context_id"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("synthetic", "real", "reference", "logs", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    require(not args.out.exists(), "Evidence report must be new")
    validation = load(args.real / "validation.json")
    worker_hash = validation["worker_sha256"]
    require(validation["completed"] is True and validation["passed"] is True and len(validation["cases"]) == 30,
            "real CLI matrix")
    require(len(validation["corruption_controls"]) == 5 and
            len(validation["baseline_comparison"]) == 10 and
            all(c["strict_exports_equal"] for c in validation["baseline_comparison"]), "strict saved exports")
    for vendor in (4318, 32902, 0):
        for label, event, instance in selected_cases("all"):
            name = f"{vendor}-{label}-{event}-{instance}"
            verify(args.real / name / "vs-inputs", args.reference / name / "vs-inputs", worker_hash,
                   validation["reference_worker_sha256"], label, event, instance, vendor)
    cases = [(v, m) for v in (0, 4318, 32902) for m in ("selected", "remaining", "no-position", "hs")]
    require({p.name for p in args.synthetic.iterdir() if p.is_dir()} == {f"{v}-{m}" for v, m in cases},
            "twelve synthetic CLI selections")
    records = [synthetic(args.synthetic / f"{v}-{m}", v, m, worker_hash) for v, m in cases]
    controls = []
    for mode, target in (("no-position", "status"), ("no-position", "vertices.bin"),
                         ("no-position", "unexpected-obj"), ("hs", "patch_constants.bin"),
                         ("hs", "vertices.validity.bin"), ("remaining", "remaining_vertices.bin")):
        with tempfile.TemporaryDirectory(prefix="flora-cli-raw-") as temporary:
            folder = Path(temporary) / "copy"
            shutil.copytree(args.synthetic / ("0-" + mode), folder)
            result = load(folder / "result.json")
            if target == "status":
                result["mesh_presentation"]["available"] = True
            elif target == "unexpected-obj":
                (folder / "post_geometry.obj").write_text("Invented position export", encoding="utf-8")
            else:
                raw = (folder / target).read_bytes()
                raw = bytes([raw[0] ^ 1]) + raw[1:]
                (folder / target).write_bytes(raw)
                result["artifacts"][target] = {"bytes": len(raw), "sha256": digest(raw)}
            (folder / "result.json").write_text(json.dumps(result), encoding="utf-8")
            try:
                synthetic(folder, 0, mode, worker_hash)
            except (ValueError, KeyError):
                controls.append(mode + ":" + target)
            else:
                raise ValueError("Accepted corrupt raw result")
    regression = {}
    for suite, expected in {"NativeReplayMeshUi": (99, 0, 0), "NativeAnalysis": (61, 0, 0),
                            "PostTransform": (13, 0, 0), "NativeComputeDebug": (68, 0, 0),
                            "GeometryUavDependency": (69, 0, 0), "ReplayMeshUi": (4, 0, 2)}.items():
        text = (args.logs / f"mesh-cli-{suite}-verified.log").read_text(encoding="utf-8")
        match = re.search(r"Totals: (\d+) passed, (\d+) failed, (\d+) skipped", text)
        require(match and tuple(map(int, match.groups())) == expected and "Finished testing" in text,
                "regression: " + suite)
        regression[suite] = dict(zip(("passed", "failed", "skipped"), expected))
    args.out.write_text(json.dumps({"schema": 1, "completed": True, "passed": True, "synthetic": records,
                                   "corruption_controls": controls, "real_cli_cases": 30,
                                   "strict_saved_exports": 10, "regression": regression}, indent=2) + "\n",
                        encoding="utf-8")
    print("12 synthetic CLI cases, 30 real CLI cases and 10 saved exports verified")


if __name__ == "__main__":
    main()
