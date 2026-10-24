"""Verify independent D3D11 stencil/MinLOD observations and pin their source identities."""

import argparse
import itertools
import json
from pathlib import Path
import struct

from summarize_dxbc_execution import identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    expected_runs = {(v, enabled, lod) for v in [0, 4318, 32902]
                     for enabled, lod in [(False, 0), (True, 0), (True, .25), (True, 1)]}
    expected_records = {(fmt, array, (x, y, z, sample))
                        for fmt, array, sample in itertools.product([22, 47], [False, True], [0, 3])
                        for x, y, z in [(1, 2, 0), (4, 0, 0), (4294967295, 0, 0), (0, 4, 0), (0, 0, 2)]}
    runs, selections, adapters, alphas, shaders = [], set(), {}, {}, {}
    for path in sorted(args.root.glob("*/results.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        selection = (data["vendor"], data["resource_clamp"], data["requested_min_lod"])
        if selection not in expected_runs or selection in selections:
            raise ValueError("Unexpected or duplicate probe selection")
        selections.add(selection)
        vendor, enabled, lod = selection
        adapter = (data["adapter_vendor"], data["adapter_device"], data["adapter_luid"])
        if vendor and adapter[0] != vendor:
            raise ValueError("Requested adapter was substituted")
        if vendor in adapters and adapters[vendor] != adapter:
            raise ValueError("Probe adapter identity changed")
        adapters[vendor] = adapter
        records = data["records"]
        if data["validation_errors"] or len(records) != 40 or {
                (r["format"], r["array"], tuple(r["address"])) for r in records} != expected_records:
            raise ValueError("Incomplete or invalid probe record matrix")
        for record in records:
            x, y, z, _ = record["address"]
            outside = x >= 4 or y >= 4 or (record["array"] and z >= 2) or (enabled and lod > 0)
            bits = record["bits"]
            if len(bits) != 4 or bits[:3] != [0, 0 if outside else 90, 0] or bits[3] not in (0, 1):
                raise ValueError("Independent stencil value disagrees with the fixture")
            if vendor in alphas and alphas[vendor] != bits[3]:
                raise ValueError("Observed alpha changed across probe cases")
            alphas[vendor] = bits[3]
        files = sorted(path.parent.glob("*.dxbc"))
        if {p.name for p in files} != {f"{fmt}-{shape}.dxbc" for fmt in [22, 47] for shape in ["2d", "array"]}:
            raise ValueError("Missing retained probe shader")
        shader_ids = []
        for shader in files:
            raw = shader.read_bytes()
            if len(raw) < 32 or raw[:4] != b"DXBC" or struct.unpack_from("<I", raw, 24)[0] != len(raw):
                raise ValueError("Malformed retained shader")
            entry = identity(shader)
            if shader.name in shaders and shaders[shader.name] != entry["sha256"]:
                raise ValueError("Probe shader changed across adapter/MinLOD runs")
            shaders[shader.name] = entry["sha256"]
            shader_ids.append(entry)
        summary = {key: value for key, value in data.items() if key != "records"}
        summary["record_count"] = len(records)
        summary["observed_values"] = sorted({tuple(record["bits"]) for record in records})
        runs.append({**identity(path), "result": summary, "shaders": shader_ids})
    if selections != expected_runs:
        raise ValueError("Incomplete adapter/MinLOD run matrix")
    report = {"schema": 1, "scope": "independent_d3d11_msaa_stencil_and_min_lod",
              "complete_release_acceptance": False, "validation_passed": True,
              "records": 480, "adapters": adapters, "observed_alpha": alphas,
              "binary": identity(args.binary), "sources": [identity(Path(p)) for p in [
                  "tools/native/msaa_stencil_probe.cpp", "tools/summarize_msaa_stencil.py"]], "runs": runs}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("Verified 12 independent runs and 480 stencil/MinLOD observations")


if __name__ == "__main__":
    main()
