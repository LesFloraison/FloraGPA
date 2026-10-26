"""Cross-check native PS journals, raster inputs and independent C++ projections."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from validate_dxbc_execution_trace import validate, negative_checks, reader_negative_checks, require


def identity(path):
    data = path.read_bytes()
    return {"path": str(path), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", nargs="+", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    runs, failures, files = [], [], set()
    for directory in args.directories:
        report = json.loads((directory / "report.json").read_text())
        files.update(p for p in directory.rglob("*") if p.is_file())
        if report["status"] != "ok":
            require("trace_error" in report and not (directory / "pixel-trace.jsonl").exists(),
                    "Failed trace was published or failure is unexplained")
            failures.append({"path": str(directory), "error": report["trace_error"],
                             "frame_sha256": report["frame_sha256"], "adapter_vendor": report["adapter_vendor"]})
            continue
        path = directory / "pixel-trace.jsonl"
        reader = json.loads((directory / "trace-reader.json").read_text())
        header = reader["header"]
        require(header["shader_sha256"] == identity(directory / "original-ps.dxbc")["sha256"], "Effective shader differs")
        require(header["quad_input_provenance"]["original_fragment_eligibility"] is False, "Invented eligibility")
        require(report["trace_sha256"] == identity(path)["sha256"], "Probe trace digest differs")
        require(report["trace_rewind_verified"] is True, "Probe did not restore its selected registers")
        initial = {}
        with path.open() as trace:
            for line in trace:
                row = json.loads(line)
                if row["record"] == "step":
                    break
                if row["record"] == "register":
                    reg = row["register"]
                    initial[(row["lane"], reg["dxbc_operand_kind"], tuple(reg["indices"]))] = row["value"]
        for meta in header["quad_input_provenance"]["passes"]:
            reg = meta["input_register"]
            p = directory / f"input-{reg}"
            require(meta == json.loads((p / "records.json").read_text()), "Trace raster provenance differs")
            require(meta["records"] == 1 and meta["shader_sha256"] == header["shader_sha256"], "Invalid input context")
            raw = (p / "records.bin").read_bytes()
            require(struct.unpack_from("<I", raw)[0] == 1, "Ambiguous raw input")
            for lane in range(4):
                require(initial[(lane, 1, (reg,))] == {
                    "bits": list(struct.unpack_from("<4I", raw, 16 + lane * 16)),
                    "valid_components": meta["valid_mask"]}, "Trace input bits/validity differ from raster observation")
        # These digests compare two journal readers. They are deliberately not
        # described as independent observations of the application's resources.
        evidence = {"frame_sha256": report["frame_sha256"], "event": report["event"],
                    "shader": header["gpa_shader"], "shader_sha256": header["shader_sha256"],
                    "adapter_luid": report["adapter_luid"], "trace": dict(report["trace"])}
        for name, space in (("buffers", "buffer"), ("textures", "texture")):
            evidence[name] = [v for v in reader["initial"]["storage"] if v["space"] == space]
            evidence["trace"][name + "_after"] = [v for v in reader["final"]["storage"] if v["space"] == space]
        checked = validate(path, evidence, reader)
        corruptions = negative_checks(path, evidence)
        reader_corruptions = reader_negative_checks(path, evidence, reader)
        runs.append({"path": str(directory), "steps": checked["steps"], "selected_steps": checked["selected_steps"],
                     "trace_sha256": checked["trace_sha256"], "rewound": checked["rewound"],
                     "corruptions": corruptions, "reader_corruptions": reader_corruptions})
    require(runs, "No completed pixel traces")
    result = {"schema": 1, "complete_release_acceptance": False,
              "scope": "input_provenance_serialization_and_reversible_projection_not_shader_output_equivalence",
              "completed_runs": len(runs), "failed_runs": len(failures), "runs": runs, "failures": failures,
              "artifacts": [identity(p) for p in sorted(files)]}
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(json.dumps({"completed_runs": len(runs), "failed_runs": len(failures),
                      "selected_steps": sum(r["selected_steps"] for r in runs)}, indent=2))


if __name__ == "__main__":
    main()
