"""Check the pinned Hellblade2 TraceFromProbesCS task-to-atlas mapping.

Offline research only (Python/numpy). Counts come from each post-event
observation, not loading-time indirect metadata. This checks tile coverage
and compares common selected probe storage; it does not emulate ray tracing,
observe GPU-consumed arguments, or certify whole-frame fidelity.
"""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import struct

import numpy as np

SHADER = "70a1a54570da8bdc500527bedbb072b636dc527a0512b1449139908f61926a88"


def load(report_path, pipeline_path):
    report = json.loads(report_path.read_text(encoding="utf-8"))
    pipeline = json.loads(pipeline_path.read_text(encoding="utf-8"))
    job = json.loads((pipeline_path.parent / "job.json").read_text(encoding="utf-8"))
    if Path(report["capture"]).resolve() != Path(job["capture"]).resolve():
        raise ValueError("Readback and pipeline captures differ")
    if (report["event"] != pipeline["eid"] or not report["repeat_only"] or
            report["history_requested"] or report["between_event"] is not None):
        raise ValueError("Expected repeat-only observations at the trace dispatch")
    shaders = [r for r in pipeline["pipeline_rows"] if r["section"] == "CS" and r["binding"] == "Shader"]
    if len(shaders) != 1 or shaders[0]["details"]["bytecode_sha256"] != SHADER:
        raise ValueError("Expected the verified TraceFromProbesCS shader")
    namespace = report.get("resource_namespace", "native_creation")
    if namespace not in ("native_creation", "rdc_id"):
        raise ValueError("Unknown resource identity namespace")
    key = "creation" if namespace == "native_creation" else "rdc_id"
    pkey = "resource_creation" if namespace == "native_creation" else "rdc_id"
    indirect = pipeline["indirect_inputs"]
    if (indirect["status"] != "read" or not indirect["action_present"] or
            indirect["action_index"] != 0 or indirect["maximum_count"] != 1 or
            indirect["effective_count"] != 1 or int(indirect["stride"]) != 12):
        raise ValueError("Expected a single dispatch-only indirect command")

    def binding(name):
        matches = [r["details"]["descriptor"] for r in pipeline["pipeline_rows"]
                   if r.get("details", {}).get("reflection_binding", {}).get("name") == name]
        if len(matches) != 1:
            raise ValueError(f"Missing unique binding: {name}")
        return matches[0]

    observations = []
    for phase in ("first", "second"):
        verified = set()

        def read(identity):
            if not isinstance(identity, str) or not identity:
                raise ValueError("Missing string resource identity")
            matches = [(i, d) for i, d in enumerate(report["resources"]) if d.get(key) == identity]
            files = [f for f in report["readback_files"] if f["phase"] == phase and f.get(key) == identity]
            if len(matches) != 1 or len(files) != 1:
                raise ValueError("Missing unique resource observation")
            i, desc = matches[0]
            path = Path(files[0]["file"])
            if not path.is_absolute():
                path = report_path.parent / path
            size = path.stat().st_size
            if not 0 < size <= 32 * 1024 * 1024 or size != int(report[phase][i]["bytes"]):
                raise ValueError("Incomplete or unbounded readback")
            raw = path.read_bytes()
            if hashlib.sha256(raw).hexdigest() != report[phase][i]["storage_sha256"]:
                raise ValueError("Readback digest differs")
            if desc["kind"] == "buffer":
                if desc["byte_offset"] != "0" or int(desc["byte_length"]) != size:
                    raise ValueError("Expected complete buffer storage")
            elif not (desc["kind"] == "texture" and desc["array_size"] == 1 and
                      desc["mips"] == 1 and desc["samples"] == 1 and
                      desc["selected_subresource"] == {"mip": 0, "layer": 0, "sample": 0}):
                raise ValueError("Expected a complete single-subresource texture")
            verified.add(identity)
            return desc, raw

        def bound(name):
            return read(binding(name)["resource"][pkey])

        _, root = bound("_RootShaderParameters")
        offset = int(binding("_RootShaderParameters")["byte_offset"])
        constants = root[offset:offset + 808]
        if (len(constants) != 808 or struct.unpack_from("<5I", constants, 384) != (128, 128, 48, 4, 16)
                or struct.unpack_from("<2I", constants, 800) != (127, 7)):
            raise ValueError("Unexpected probe/atlas layout")
        _, args = read(indirect["argument_buffer"][pkey])
        groups = struct.unpack_from("<3I", args, int(indirect["argument_offset"]))
        _, counter = bound("ProbeTraceTileAllocator")
        if len(counter) != 4:
            raise ValueError("Expected a scalar tile allocator")
        count, = struct.unpack("<I", counter)
        _, raw_tiles = bound("ProbeTraceTileData")
        _, raw_tasks = bound("ProbeTraceData")
        if len(raw_tiles) != 2097152 or len(raw_tasks) != 262144:
            raise ValueError("Unexpected task allocations")
        if not 0 < count <= len(raw_tiles) // 8 or groups != (128, (count + 127) // 128, 1):
            raise ValueError("Tile allocator does not match the observed indirect extent")
        tiles = np.frombuffer(raw_tiles, dtype="<u4").reshape(-1, 2)[:count]
        tasks = np.frombuffer(raw_tasks, dtype="<u4").reshape(-1, 4)
        if np.any(tiles[:, 1] >= len(tasks)):
            raise ValueError("Tile references an out-of-bounds trace task")
        # Shader: (packed & 255, (packed >> 8) & 255), resolution=8<<(packed>>16).
        # Restrict this sample checker to its observed levels; do not silently
        # accept a new level, masked high bits, or overlapping output coverage.
        x = tiles[:, 0] & 255
        y = (tiles[:, 0] >> 8) & 255
        levels = tiles[:, 0] >> 16
        if np.any(levels > 2):
            raise ValueError("Unsupported trace resolution level")
        selected = {}
        for task_index in np.unique(tiles[:, 1]):
            mask = tiles[:, 1] == task_index
            task_levels = np.unique(levels[mask])
            if len(task_levels) != 1:
                raise ValueError("One task has inconsistent trace resolution levels")
            level = int(task_levels[0])
            side = 1 << level
            coords = list(zip(x[mask].tolist(), y[mask].tolist()))
            if len(coords) != side * side or set(coords) != set(itertools.product(range(side), repeat=2)):
                raise ValueError("Trace task has missing, duplicate or out-of-range tiles")
            task = tasks[task_index]
            probe = int(task[3]) & 0xffffff
            clipmap = int(task[3]) >> 24
            if probe >= 16384 or clipmap >= 4 or probe in selected:
                raise ValueError("Invalid or duplicate selected probe")
            selected[probe] = {"level": level, "position_bits": task[:3].tolist(), "clipmap": clipmap}

        # The grid is a separately supplied identity (it is not bound by this
        # shader). Require the exact sampled volume shape/format and unique ID.
        grids = [d for d in report["resources"] if d["kind"] == "texture" and d.get("dimension") == 3]
        if len(grids) != 1:
            raise ValueError("Expected one explicit cache grid")
        desc, grid_raw = read(grids[0].get(key))
        if ((desc["width"], desc["height"], desc["depth"], desc["format_type"],
             desc["component_count"], desc["component_bytes"]) != (192, 48, 48, 0, 1, 4)
                or desc["readback_scope"] != "whole_3d_mip" or len(grid_raw) != 192 * 48 * 48 * 4):
            raise ValueError("Unexpected cache-grid storage")
        grid = np.frombuffer(grid_raw, dtype="<u4")
        cells = np.flatnonzero(grid != 0xffffffff)
        ids = grid[cells]
        if np.any(ids >= 16384) or len(np.unique(ids)) != len(ids) or not set(selected) <= set(ids):
            raise ValueError("Invalid active cache-grid mapping")
        atlases = {}
        for name, short, dtype in (("RWRadianceProbeAtlasTexture", "radiance", "<u4"),
                                    ("RWDepthProbeAtlasTexture", "depth", "<u2")):
            desc, raw = bound(name)
            if ((desc["dimension"], desc["width"], desc["height"], desc["depth"]) != (2, 2048, 2048, 1) or
                    desc["format_type"] != (13 if short == "radiance" else 0) or
                    desc["component_count"] != (3 if short == "radiance" else 1)):
                raise ValueError("Unexpected output atlas format")
            atlas = np.frombuffer(raw, dtype=dtype).reshape(2048, 2048)
            atlases[short] = atlas.reshape(128, 16, 128, 16).transpose(0, 2, 1, 3).reshape(16384, 256)[ids].copy()
        by_cell = {int(cell): selected[int(probe)] for cell, probe in zip(cells, ids) if int(probe) in selected}
        if any((cell % 192) // 48 != task["clipmap"] for cell, task in by_cell.items()):
            raise ValueError("Selected task clipmap differs from its cache-grid cell")
        inputs = {}
        for name in ("FinalLightingAtlas", "DepthAtlas", "UniformBufferConstants_View",
                     "UniformBufferConstants_LumenCardScene", "UniformBufferConstants_ReflectionStruct",
                     "ProbeWorldOffset"):
            descriptor = binding(name)
            identity = descriptor["resource"][pkey]
            if not any(d.get(key) == identity for d in report["resources"]):
                inputs[name] = {"status": "not_observed"}
                continue
            desc, raw = read(identity)
            if desc["kind"] == "texture":
                width = 4 if desc["format_type"] == 13 else desc["component_bytes"] * desc["component_count"]
                if (desc["dimension"] != 2 or desc["depth"] != 1 or width <= 0 or
                        len(raw) != desc["width"] * desc["height"] * width):
                    raise ValueError("Expected a complete uncompressed input atlas")
                layout = {k: desc[k] for k in ("width", "height", "format_type", "component_count", "component_bytes")}
            else:
                layout = {"buffer_bytes": len(raw)}
            inputs[name] = {"status": "read", "sha256": hashlib.sha256(raw).hexdigest(),
                            "bytes": len(raw), "layout": layout,
                            "binding_byte_offset": descriptor["byte_offset"],
                            "scope": "Complete selected resource storage; constant-buffer hashes include bytes outside the bound block"}
        observations.append({"phase": phase, "namespace": namespace, "groups": list(groups),
                             "tile_count": count, "tasks_by_cell": by_cell, "cells": cells, "ids": ids,
                             "atlases": atlases, "constants": constants, "inputs": inputs,
                             "verified_resource_observations": len(verified)})
    return observations


def compare(observations):
    comparisons = []
    for (an, a), (bn, b) in itertools.combinations(observations, 2):
        if not np.array_equal(a["cells"], b["cells"]):
            raise ValueError("Active grid-cell domains differ; separate alignment is required")
        common = sorted(a["tasks_by_cell"].keys() & b["tasks_by_cell"].keys())
        positions = np.searchsorted(a["cells"], common)
        task_differences = [cell for cell in common if a["tasks_by_cell"][cell] != b["tasks_by_cell"][cell]]
        differences = {}
        for name in ("radiance", "depth"):
            left = a["atlases"][name][positions]
            right = b["atlases"][name][positions]
            different = left != right
            differences[name] = {"blocks": int(np.count_nonzero(np.any(different, axis=1))),
                                 "texels": int(np.count_nonzero(different)),
                                 "different_blocks_with_same_probe_id": int(np.count_nonzero(
                                     np.any(different, axis=1) & (a["ids"][positions] == b["ids"][positions])))}
            if name == "radiance" and len(positions):
                # Packed channel code distances, not float-value errors or an
                # acceptance tolerance. Preserve every nonzero difference.
                differences[name]["max_packed_component_code_delta"] = [
                    int(np.max(np.abs(((left >> shift) & mask).astype(np.int64) -
                                      ((right >> shift) & mask).astype(np.int64))))
                    for shift, mask in ((0, 2047), (11, 2047), (22, 1023))]
        comparisons.append({"a": an, "b": bn, "common_selected_cells": len(common),
                            "selected_task_metadata_differences": len(task_differences),
                            "common_selected_probe_id_differences": int(np.count_nonzero(a["ids"][positions] != b["ids"][positions])),
                            "root_parameters_equal": a["constants"] == b["constants"], "output_differences": differences,
                            "input_comparisons": {name: "not_observed_in_both" if
                                a["inputs"][name]["status"] != "read" or b["inputs"][name]["status"] != "read" else
                                "equal" if a["inputs"][name] == b["inputs"][name] else "different"
                                for name in a["inputs"]},
                            "all_shader_inputs_observed": False})
    return comparisons


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("native_report", "native_pipeline", "original_report", "original_pipeline", "out"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    observations = []
    for label in ("native", "original"):
        for row in load(getattr(args, label + "_report"), getattr(args, label + "_pipeline")):
            if row["namespace"] != ("native_creation" if label == "native" else "rdc_id"):
                raise ValueError("Native/original resource identity namespaces are reversed")
            observations.append((label + "-" + row["phase"], row))
    result = {"scope": __doc__, "shader_sha256": SHADER, "observations": [], "comparisons": compare(observations)}
    for label, row in observations:
        result["observations"].append({"label": label, "namespace": row["namespace"],
            "groups": row["groups"], "tile_count": row["tile_count"],
            "active_grid_cells": len(row["cells"]),
            "selected_tasks": len(row["tasks_by_cell"]),
            "tasks_by_resolution": {str(8 << level): sum(t["level"] == level for t in row["tasks_by_cell"].values())
                                    for level in range(3)},
            "input_observations": row["inputs"],
            "verified_resource_observations": row["verified_resource_observations"]})
    with args.out.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
