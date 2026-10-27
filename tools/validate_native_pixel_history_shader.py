"""Validate private raster-input/checkpoint/shader-output joins without external tools."""
import argparse
import copy
import json
import hashlib
import struct
from pathlib import Path
from validate_native_pixel_draw_history import validate as history, require
from validate_native_pixel_outputs import common as outputs
from validate_native_pixel_geometry_replay import FRAME_HASHES

VENDORS = (0, 4318, 32902)
MODES = ("constant", "coverage", "varying", "derivative", "sample", "masked", "discard")
UAVS = ("counter", "append", "raw", "typed", "texture")


def validate(r, vendor):
    history(r, vendor)
    stats = r["shader_evaluation"]
    require(stats["requested"] is True and stats["original_fragment_eligibility"] is None and
            stats["resource_boundary"] == "before_private_primitive", "evaluation scope")
    count = steps = 0
    adapter = r["adapter"]
    luid = (adapter["luid_high"] << 32) | adapter["luid_low"]
    for row in r["records"]:
        values = row["private_shader_outputs"]
        require(bool(values) == row["pixel_shader_bound"], "bound shader evaluation")
        covered = []
        for out in values:
            outputs(out)
            count += 1
            steps += out["global_steps"]
            require(out["resource_observation"] == "current_native_bindings" and
                    out["resource_boundary"] == "before_private_primitive" and
                    out["completed_primitives"] == row["primitive_ordinal"], "resource boundary")
            require(out["frame_sha256"] == r["frame_sha256"] and out["event"] == r["event"] and
                    out["shader_sha256"] == r["geometry_replay"]["pixel_shader_sha256"] and
                    out["adapter"] == dict(vendor=adapter["vendor"], device=adapter["device"], luid=luid),
                    "execution context")
            provenance = out["input_provenance"]
            require(provenance["source"] == "unique-private-raster-quad/1" and
                    provenance["scope"] == "isolated_primitive_pre_depth_inputs" and
                    provenance["original_fragment_eligibility"] is None, "input scope")
            observations = [provenance] + provenance.get("equivalent_sample_observations", [])
            require(len(observations) == len(out["candidate_raster_samples"]), "sample acquisition coverage")
            for sample, p in zip(out["candidate_raster_samples"], observations):
                covered.append(sample)
                require(p["geometry_sha256"] == r["geometry_replay"]["geometry"]["sha256"] and
                        p["primitive_ordinal"] == row["primitive_ordinal"] and
                        p["original_primitive_id"] == row["original_primitive_id"], "geometry join")
                require(len(p["passes"]) > 0, "missing input observation")
                for observation in p["passes"]:
                    require(observation["records"] == 1 and observation["shader_sha256"] == out["shader_sha256"]
                            and observation["adapter_luid"] == luid and observation["adapter_vendor"] == adapter["vendor"]
                            and observation["adapter_device"] == adapter["device"], "observed shader/adapter")
                    require((observation["x"], observation["y"], observation["sample"], observation["layer"]) ==
                            (r["selection"]["x"], r["selection"]["y"], sample,
                             r["selection"]["subresource"]["relative_layer"]), "observed selection")
                    require(observation["geometry"]["primitive_ordinal"] == row["primitive_ordinal"] and
                            observation["geometry"]["original_primitive_id"] == row["original_primitive_id"], "pass geometry")
                    if p["coverage_only"]:
                        require(observation["input_register"] == 0xffffffff and observation["valid_mask"] == 0,
                                "coverage-only observation")
                if p["sample_frequency"]:
                    require(len(observations) == 1, "sample-frequency invocations were merged")
        if row["pixel_shader_bound"]:
            require(sorted(covered) == sorted({x["raster_sample"] for x in row["observations"]}), "sample join")
    require(stats["invocations"] == count and stats["global_steps"] == steps and
            stats["retained_payload_bytes"] >= 0, "aggregate execution")


def fixture(name, r, vendor):
    validate(r, vendor)
    mode = r.get("mode")
    for row in r["records"]:
        for out in row["private_shader_outputs"]:
            lane = out["lanes"][out["selected_lane"]]
            value = lane["outputs"][0]
            if mode == "discard":
                require(lane["discarded"] is True, "discard result")
                continue
            require(value["valid_mask"] == 15, "selected output validity")
            if mode in UAVS:
                expected = [7 if mode == "append" else row["primitive_ordinal"], 11, 17, 23]
                require(value["raw_bits"] == expected and list(struct.unpack("<4I",bytes(row["after"]["bytes"]))) == expected,
                        "current UAV/counter output")
            elif "reference_gpu_bits" in r and mode != "masked":
                require(value["raw_bits"] == r["reference_gpu_bits"], "independent native output")
            if mode in ("constant", "coverage") or mode in UAVS:
                require(out["input_provenance"]["coverage_only"] is True, "coverage-only input path")
            if mode == "sample":
                require(out["input_provenance"]["sample_frequency"] is True, "sample-frequency input path")
            if mode == "derivative":
                require(out["derivative_batches"] > 0, "derivative execution")
            if mode in ("masked", "depth_reject", "stencil_reject"):
                require(row["observed_value_changed"] is False, "rejected storage stays unchanged")
    if mode == "null_ps":
        require(r["shader_evaluation"]["invocations"] == 0 and len(r["records"]) == 1, "null PS storage")
    if name.startswith("shaderHistoryRecovery"):
        require(r["failure_recovery"] is True and r["shader_evaluation"]["invocations"] == 2, "recovery")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("geometry", type=Path)
    parser.add_argument("checkpoint", type=Path)
    parser.add_argument("--real-prefix", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    expected = {f"shaderHistory-{v}-{m}-{s}": (args.geometry, v) for v in VENDORS for m in MODES for s in (0,3)}
    expected.update({f"shaderHistoryBoundaries-{v}-{m}": (args.geometry, v) for v in VENDORS
                     for m in ("depth_reject", "stencil_reject", "null_ps", "forced")})
    expected.update({f"shaderHistoryRecovery-{v}": (args.geometry, v) for v in VENDORS})
    expected.update({f"shaderHistory-{v}-{m}": (args.checkpoint, v) for v in VENDORS for m in UAVS})
    cases = []
    for name, (directory, vendor) in sorted(expected.items()):
        r = json.loads((directory / (name + ".json")).read_text(encoding="utf-8"))
        require(r["status"] == "ok", "fixture status")
        fixture(name, r, vendor)
        cases.append((name, r, vendor))
    for game in ("gf2", "bf1", "hd"):
        for vendor in VENDORS:
            directory = Path(str(args.real_prefix)+f"-{game}-{vendor}")
            r = json.loads((directory / "report.json").read_text(encoding="utf-8"))
            validate(r, vendor)
            require(r["probe"]["status"] == "ok" and r["frame_sha256"] == FRAME_HASHES[game], "real capture identity")
            event, x, y, records, steps = {"gf2": (113, 799, 480, 1, 100), "bf1": (2010, 751, 433, 10, 4080),
                                         "hd": (200, 168, 183, 1, 1316)}[game]
            require((r["event"], r["selection"]["x"], r["selection"]["y"], len(r["records"]),
                     r["shader_evaluation"]["global_steps"]) == (event, x, y, records, steps), "real selection/execution")
            reference = (directory / "reference.bin").read_bytes()
            private = (directory / "private.bin").read_bytes()
            require(reference == private == bytes(r["after"]["bytes"]) and
                    hashlib.sha256(reference).hexdigest() == r["probe"]["reference_sha256"] == r["probe"]["private_sha256"],
                    "real selected storage comparison")
            cases.append((f"{game}-{vendor}", r, vendor))
    controls = 0
    for name, report, vendor in cases:
        changes = [lambda r: r["shader_evaluation"].update(global_steps=-1),
                   lambda r: r.update(original_fragment_eligibility=True)]
        if report["shader_evaluation"]["invocations"]:
            changes += [lambda r: r["records"][0]["private_shader_outputs"][0].update(resource_observation="before_event_replay"),
                        lambda r: r["records"][0]["private_shader_outputs"][0].update(completed_primitives=999999),
                        lambda r: r["records"][0]["private_shader_outputs"][0]["input_provenance"].update(geometry_sha256="0"*64),
                        lambda r: r["records"][0]["private_shader_outputs"][0]["input_provenance"]["passes"][0].update(sample=31)]
        for change in changes:
            broken = copy.deepcopy(report); change(broken)
            try:
                validate(broken, vendor)
            except ValueError:
                controls += 1
            else:
                raise ValueError("Accepted corrupt join: " + name)
    result = dict(schema=1, status="ok", owned_cases=len(expected), real_cases=9, corruption_controls=controls,
                  complete_pixel_history=False, public_pixel_debugging=False)
    with args.out.open("x",encoding="utf-8") as f:
        json.dump(result,f,indent=2);f.write("\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
