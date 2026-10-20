"""Offline exact check of the pinned Hellblade2 cache-border shader.

Uses each observation's post-event argument buffer and task prefix. These are
not a GPU argument-consumption log. An explicit before/after event pair also
checks preservation outside the selected probe blocks, while reporting changing
upstream inputs separately. This does not establish original/native whole-frame
fidelity. Python/numpy is research-only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

import numpy as np

SHADER = "d57fdb753b2c4a0f4ab57f2ec9254ae156b4b78bde5b0e4821b4cb0c3f961b85"


def analyze(report_path, pipeline_path):
    report = json.loads(report_path.read_text(encoding="utf-8"))
    pipeline = json.loads(pipeline_path.read_text(encoding="utf-8"))
    shader = next(row for row in pipeline["pipeline_rows"]
                  if row["section"] == "CS" and row["binding"] == "Shader")
    if shader["details"]["bytecode_sha256"] != SHADER:
        raise ValueError("Expected the verified FixupBordersAndGenerateMipsCS shader")
    boundaries = "second_event" in report
    if not report["repeat_only"] or report["history_requested"] or report["between_event"] is not None:
        raise ValueError("Expected bounded observations without Pixel History or intervening events")
    if boundaries:
        if (report["second_event"] != pipeline["eid"] or
                pipeline["indirect_selection"]["marker_eids"] != [report["event"]]):
            raise ValueError("Expected the selected execution marker and its indirect dispatch")
    elif report["event"] != pipeline["eid"]:
        raise ValueError("Expected repeated observations at the selected border dispatch")
    job = json.loads((pipeline_path.parent / "job.json").read_text(encoding="utf-8"))
    if Path(job["capture"]).resolve() != Path(report["capture"]).resolve():
        raise ValueError("Pipeline and readback captures differ")
    namespace = report.get("resource_namespace", "native_creation")
    if namespace not in ("native_creation", "rdc_id"):
        raise ValueError("Unknown resource identity namespace")
    key = "rdc_id" if namespace == "rdc_id" else "creation"
    pipeline_key = "rdc_id" if namespace == "rdc_id" else "resource_creation"
    indirect = pipeline["indirect_inputs"]
    if (indirect["status"] != "read" or indirect["action_index"] != 0 or
            indirect["maximum_count"] != 1 or indirect["effective_count"] != 1 or
            not indirect["action_present"] or int(indirect["stride"]) != 12):
        raise ValueError("Expected a single dispatch-only indirect command")

    def binding(name):
        found = [r["details"]["descriptor"] for r in pipeline["pipeline_rows"]
                 if r.get("details", {}).get("reflection_binding", {}).get("name") == name]
        if len(found) != 1:
            raise ValueError(f"Missing unique shader binding: {name}")
        return found[0]

    verified = set()

    def read(phase, identity):
        if not isinstance(identity, str):
            raise ValueError("Missing string resource identity")
        matches = [(i, d) for i, d in enumerate(report["resources"]) if d.get(key) == identity]
        files = [f for f in report["readback_files"] if f["phase"] == phase and f.get(key) == identity]
        if len(matches) != 1 or len(files) != 1:
            raise ValueError("Missing unique bounded resource observation")
        i, desc = matches[0]
        path = Path(files[0]["file"])
        if not path.is_absolute():
            path = report_path.parent / path
        observed = report[phase][i]
        size = path.stat().st_size
        if not 0 < size <= 32 * 1024 * 1024 or size != int(observed["bytes"]):
            raise ValueError("Incomplete or unbounded resource readback")
        raw = path.read_bytes()
        if hashlib.sha256(raw).hexdigest() != observed["storage_sha256"]:
            raise ValueError("Readback digest differs")
        if desc["kind"] == "buffer":
            if desc["byte_offset"] != "0" or int(desc["byte_length"]) != size:
                raise ValueError("Expected complete buffer storage")
        elif not (desc["kind"] == "texture" and desc["dimension"] == 2 and
                  desc["array_size"] == 1 and desc["mips"] == 1 and desc["samples"] == 1 and
                  desc["format_type"] == 13 and desc["component_count"] == 3 and
                  desc["selected_subresource"] == {"mip": 0, "layer": 0, "sample": 0} and
                  size == desc["width"] * desc["height"] * 4):
            raise ValueError("Expected the complete packed R11G11B10 atlas")
        verified.add((phase, identity))
        return desc, raw

    def read_binding(phase, name):
        return read(phase, binding(name)["resource"][pipeline_key])

    rows = []
    boundary_evidence = None
    for phase in (("second",) if boundaries else ("first", "second")):
        _, root = read_binding(phase, "_RootShaderParameters")
        offset = int(binding("_RootShaderParameters")["byte_offset"])
        layout = struct.unpack_from("<7I", root, offset + 64)
        mask, shift = struct.unpack_from("<2I", root, offset + 480)
        if layout != (128, 128, 48, 4, 16, 18, 0) or (mask, shift) != (127, 7):
            raise ValueError("Unexpected border/atlas layout")
        _, arguments = read(phase, indirect["argument_buffer"][pipeline_key])
        groups = struct.unpack_from("<3I", arguments, int(indirect["argument_offset"]))
        if groups[:2] != (3, 3) or not 0 < groups[2] <= 128 * 128:
            raise ValueError("Unexpected observed border dispatch extent")
        _, data = read_binding(phase, "ProbeTraceData")
        if len(data) != 128 * 128 * 16:
            raise ValueError("Unexpected trace-data allocation")
        ids = np.frombuffer(data, dtype="<u4").reshape(-1, 4)[:groups[2], 3] & 0xFFFFFF
        if len(np.unique(ids)) != len(ids) or np.any(ids >= 128 * 128):
            raise ValueError("Invalid or repeated probe IDs in observed task prefix")
        source_desc, source = read_binding(phase, "RadianceProbeAtlasTexture")
        target_desc, target = read_binding(phase, "RWFinalRadianceAtlasMip0")
        if ((source_desc["width"], source_desc["height"]) != (2048, 2048) or
                (target_desc["width"], target_desc["height"]) != (2304, 2304)):
            raise ValueError("Unexpected source/target atlas dimensions")
        source = np.frombuffer(source, dtype="<u4").reshape(2048, 2048)
        target = np.frombuffer(target, dtype="<u4").reshape(2304, 2304)
        # Transcribe the shader's four ordered integer reflection branches,
        # then remove the one-texel border. Corners traverse both axes.
        y, x = np.indices((18, 18))
        sx, sy = x.copy(), y.copy()
        test = sx < 1
        sx, sy = np.where(test, 1 - sx, sx), np.where(test, 17 - sy, sy)
        test = sx >= 17
        sx, sy = np.where(test, 33 - sx, sx), np.where(test, 17 - sy, sy)
        test = sy < 1
        sx, sy = np.where(test, 17 - sx, sx), np.where(test, 1 - sy, sy)
        test = sy >= 17
        sx, sy = np.where(test, 17 - sx, sx), np.where(test, 33 - sy, sy)
        sx, sy = sx - 1, sy - 1
        if np.any(sx < 0) or np.any(sy < 0) or np.any(sx >= 16) or np.any(sy >= 16):
            raise ValueError("Invalid reflected source coordinate")
        px, py = (ids & mask)[:, None, None], (ids >> shift)[:, None, None]
        expected = source[py * 16 + sy, px * 16 + sx]
        actual = target[py * 18 + y, px * 18 + x]
        if (np.any(((expected >> 6) & 31) == 31) or np.any(((expected >> 17) & 31) == 31)
                or np.any(((expected >> 27) & 31) == 31)):
            raise ValueError("Non-finite packed values need separate load/store conversion analysis")
        different = int(np.count_nonzero(expected != actual))
        if different:
            raise ValueError(f"{phase}: {different} selected border texels differ from the shader mapping")
        rows.append({"phase": phase, "post_event_groups": list(groups),
                     "unique_task_count": len(ids), "checked_texels": int(actual.size),
                     "different_texels": different})
        if boundaries:
            _, prior = read_binding("first", "RWFinalRadianceAtlasMip0")
            before = np.frombuffer(prior, dtype="<u4").reshape(2304, 2304)
            selected = np.zeros(target.shape, dtype=bool)
            selected[py * 18 + y, px * 18 + x] = True
            changed = before != target
            outside = int(np.count_nonzero(changed & ~selected))
            if outside:
                raise ValueError(f"{outside} target texels changed outside the selected probe blocks")
            inputs = {}
            for name in ("_RootShaderParameters", "ProbeTraceData", "RadianceProbeAtlasTexture"):
                _, first = read_binding("first", name)
                _, second = read_binding("second", name)
                inputs[name] = "equal" if first == second else "different"
            _, first_args = read("first", indirect["argument_buffer"][pipeline_key])
            boundary_evidence = {
                "before_event": report["event"], "after_event": report["second_event"],
                "selected_probe_ids": ids.tolist(),
                "selected_texels": int(np.count_nonzero(selected)),
                "changed_selected_texels": int(np.count_nonzero(changed & selected)),
                "outside_texels": int(np.count_nonzero(~selected)),
                "changed_outside_texels": outside,
                "upstream_full_storage": inputs,
                "before_groups": list(struct.unpack_from("<3I", first_args, int(indirect["argument_offset"]))),
                "scope": "After-event source/task mapping and exact target preservation outside those blocks. "
                         "Separate prefix replays may change upstream inputs; equality is not assumed."
            }
    return {"status": "exact_selected_border_copy_and_boundary_preservation" if boundaries else "exact_selected_border_copy",
            "scope": __doc__, "observation_mode": "event_boundaries" if boundaries else "repeat_selection",
            "boundary_evidence": boundary_evidence,
            "resource_namespace": namespace, "shader_sha256": SHADER, "observations": rows,
            "verified_resource_observations": len(verified)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("report", "pipeline", "out"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    result = analyze(args.report, args.pipeline)
    with args.out.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
