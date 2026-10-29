"""Verify synthetic native feedback traces without waiving hardware oracle failures."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import struct
import subprocess

from validate_dxbc_execution_trace import validate, negative_checks, reader_negative_checks
from validate_feedback_relocation import instructions


def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--reader", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    cases, shaders, corruptions, status_changes = [], {}, 0, 0
    matrix = list(itertools.product((0, 4318, 32902), range(3), (0, 1), (0, 1)))
    expected = {"-".join(map(str, row)) for row in matrix}
    require({p.name for p in args.root.iterdir() if p.is_dir()} == expected, "Incomplete feedback matrix")
    for vendor, kind, uav, known in matrix:
        name = f"{vendor}-{kind}-{uav}-{known}"
        folder = args.root / name
        report = json.loads((folder / "report.json").read_text())
        trace_path, shader_path = folder / "trace.jsonl", folder / "shader.dxbc"
        rows = [json.loads(line) for line in trace_path.read_text().splitlines()]
        header = rows[0]
        require(report["scope"] == header["resource_observation"] == "synthetic_feedback_fixture",
                "Fixture must not claim captured invocation provenance")
        require(header["version"] == 2 and header["adapter"]["vendor"] == (vendor or 5140),
                "Wrong trace version or adapter")
        require(report["type"] == kind and report["uav"] is bool(uav) and report["non_tiled"] is bool(known),
                "Fixture identity mismatch")
        digest = hashlib.sha256(shader_path.read_bytes()).hexdigest()
        require(digest == report["shader_sha256"] == header["shader_sha256"], "Shader identity mismatch")
        require(shaders.setdefault((kind, uav), digest) == digest, "Adapter or knowledge changed shader bytes")
        code = instructions(shader_path)
        table = [r for r in rows if r["record"] == "instruction"]
        require([r["words"] for r in table] == code, "Trace instruction table differs from DXBC")
        opcode = (225 if uav else 223) if kind == 0 else 225 + kind
        require(sum((r[0] & 2047) == opcode for r in code) == 1 and
                sum((r[0] & 2047) == 234 for r in code) == 1, "Expected one feedback load and one check")
        expected_data = ([102, 0, 0, 1], [102] * 4, [108, 109, 110, 111])[kind]
        require(report["words"][:4] == expected_data and report["words"][5:] == [99, 0x0011e000, 123],
                "Data or sentinel mismatch")
        require(not known or report["words"][4] == 1, "Non-tiled input must be fully mapped")
        require(report["validity"] == [int(bool(known)) if 16 <= i < 20 else 1 for i in range(32)],
                "Mapping knowledge was confused with data validity")
        source_hash = hashlib.sha256(struct.pack("<32I", *range(100, 132))).hexdigest()
        for collection in (report["buffers"], report["trace"]["buffers_after"]):
            require(next(b for b in collection if b["resource"] == 1)["sha256"] == source_hash,
                    "Source buffer changed")
        changes = 0
        for row in rows:
            if row["record"] != "step":
                continue
            for change in row["register_changes"]:
                for c, tag in enumerate(change["after"]["mapping_status"]):
                    if tag:
                        require(tag == (2 if known else 1) and change["after"]["bits"][c] == 0 and
                                not (change["after"]["valid_components"] & (1 << c)),
                                "Opaque mapping token has invented bits or incorrect provenance")
                        changes += 1
        require(changes > 0, "Missing opaque feedback status transition")
        reader_path = folder / "reader.json"
        command = [str(args.reader.resolve()), str(trace_path.resolve()),
                   hashlib.sha256(trace_path.read_bytes()).hexdigest(), str(reader_path.resolve()),
                   "--shader", str(shader_path.resolve())]
        process = subprocess.run(command, capture_output=True, text=True, timeout=60)
        require(process.returncode == 0, f"Reader failed for {name}: {process.stderr}")
        reader = json.loads(reader_path.read_text())
        result = validate(trace_path, report, reader)
        negatives = negative_checks(trace_path, report)
        reader_negatives = reader_negative_checks(trace_path, report, reader)
        for filename, data in (("validation.json", result), ("negative.json", negatives),
                               ("reader-negative.json", reader_negatives)):
            (folder / filename).write_text(json.dumps(data, indent=2) + "\n")
        corruptions += len(negatives["cases"]) + len(reader_negatives["cases"])
        status_changes += changes
        cases.append(dict(case=name, steps=result["steps"], opaque_changes=changes,
                          shader_sha256=digest, trace_sha256=hashlib.sha256(trace_path.read_bytes()).hexdigest()))
    result = dict(schema=1, scope="synthetic_native_feedback_status", passed=True,
                  cases=cases, distinct_shaders=len(shaders), opaque_changes=status_changes,
                  rejected_corruptions=corruptions, hardware_oracle_failures_waived=False,
                  full_native_backend_acceptance=False)
    args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k != "cases"} | {"cases": len(cases)}))


if __name__ == "__main__":
    main()
