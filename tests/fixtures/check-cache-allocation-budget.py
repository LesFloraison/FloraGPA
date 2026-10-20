"""Offline check of the observed Hellblade2 AllocateProbeTracesCS budget.

The before/after reports are independent replay pairs, not a paired execution.
This checks compatibility with the exported shader's order-dependent budget
rule; it does not validate original-GPA fidelity or reproduce GPU scheduling.
Explicit --rdc-ids mode checks unmarked captures using their API boundaries
and capture-local identities; it never assigns Flora execution/creation IDs.
Python/numpy is a research dependency only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

import numpy as np


SHADER = "7894a72075225484a9559207da1c8644081135bf028b707b2458c7413793acc0"


def analyze(before_path, after_path, pipeline_path, rdc_ids=False):
    pipeline = json.loads(pipeline_path.read_text(encoding="utf-8"))
    shader = next(row for row in pipeline["pipeline_rows"]
                  if row["section"] == "CS" and row["binding"] == "Shader")
    if shader["details"]["bytecode_sha256"] != SHADER:
        raise ValueError("This model requires the verified AllocateProbeTracesCS bytecode")

    def binding(name):
        matches = [row["details"]["descriptor"] for row in pipeline["pipeline_rows"]
                   if row.get("details", {}).get("reflection_binding", {}).get("name") == name]
        if len(matches) != 1:
            raise ValueError(f"Expected exactly one reflected binding: {name}")
        return matches[0]

    identity_key = "rdc_id" if rdc_ids else "creation"
    namespace = "rdc_id" if rdc_ids else "native_creation"

    def identity(name):
        resource = binding(name)["resource"]
        value = resource["rdc_id" if rdc_ids else "resource_creation"]
        if not isinstance(value, str) or not value:
            raise ValueError("Missing string resource identity in the selected namespace")
        return value

    reports = [json.loads(path.read_text(encoding="utf-8")) for path in (before_path, after_path)]
    if reports[0]["event"] + 1 != pipeline["eid"] or reports[1]["event"] != pipeline["eid"]:
        raise ValueError("Expected adjacent boundaries around the selected dispatch")
    if rdc_ids:
        # A GPA-produced RDC has no Flora markers. Use the actual API event
        # and the containing action, without manufacturing an execution ID.
        api = reports[1].get("selection_api", {})
        preceding = reports[0].get("selection_api", {})
        if (pipeline.get("execution") is not None or
                api.get("name") != "ID3D12GraphicsCommandList::Dispatch" or
                api.get("event") != pipeline["eid"] or api.get("action_event") != pipeline["eid"] or
                preceding.get("event") != reports[0]["event"] or
                preceding.get("action_event") != pipeline["eid"] or
                not preceding.get("name") or preceding["name"] == api["name"]):
            raise ValueError("Expected an unmapped direct dispatch and its preceding API event")
        job = json.loads((pipeline_path.parent / "job.json").read_text(encoding="utf-8"))
        capture = Path(job["capture"]).resolve()
        if any(Path(report["capture"]).resolve() != capture for report in reports):
            raise ValueError("Boundary and pipeline capture paths differ")
    elif not reports[0].get("selection_action_name", "").startswith(
            f"Flora DX12 execution {pipeline['execution']} record "):
        raise ValueError("Expected the selected dispatch marker and its API event")
    for report in reports:
        if report.get("resource_namespace", "native_creation") != namespace:
            raise ValueError("Resource identity namespace mismatch")
        if not report["repeat_only"] or report["history_requested"] or report["between_event"] is not None:
            raise ValueError("Expected repeat-only observations without an intervening event")
    verified = {}

    def read(which, phase, name):
        key = (which, phase, name)
        if key in verified:
            return verified[key]
        report = reports[which]
        resource = identity(name)
        matches = [(i, d) for i, d in enumerate(report["resources"]) if d[identity_key] == resource]
        files = [f for f in report["readback_files"] if f["phase"] == phase and f[identity_key] == resource]
        if len(matches) != 1 or len(files) != 1:
            raise ValueError(f"Missing unique observation: {key}")
        index, description = matches[0]
        path = Path(files[0]["file"])
        if not path.is_absolute():
            path = (before_path if which == 0 else after_path).parent / path
        size = path.stat().st_size
        observation = report[phase][index]
        if not 0 < size <= 32 * 1024 * 1024 or size != int(observation["bytes"]):
            raise ValueError("Incomplete or unbounded readback")
        raw = path.read_bytes()
        if hashlib.sha256(raw).hexdigest() != observation["storage_sha256"]:
            raise ValueError("Readback digest mismatch")
        if description["kind"] == "buffer":
            if description["byte_offset"] != "0" or int(description["byte_length"]) != size:
                raise ValueError("Expected full buffer storage")
        elif not (description["kind"] == "texture" and description["dimension"] == 3 and
                  description["readback_scope"] == "whole_3d_mip" and description["mips"] == 1 and
                  description["array_size"] == 1 and description["samples"] == 1 and
                  (description["width"], description["height"], description["depth"]) == (192, 48, 48) and
                  size == 192 * 48 * 48 * 4):
            raise ValueError("Expected the verified complete cache-indirection volume")
        verified[key] = raw
        return raw

    def words(which, phase, name):
        return np.frombuffer(read(which, phase, name), dtype="<u4")

    def scalar(which, phase, name):
        data = words(which, phase, name)
        if len(data) != 1:
            raise ValueError(f"Expected one counter word: {name}")
        return int(data[0])

    root_name, view_name = "_RootShaderParameters", "UniformBufferConstants_View"
    root = read(0, "first", root_name)
    root_offset = int(binding(root_name)["byte_offset"])
    view_offset = int(binding(view_name)["byte_offset"])
    for which in (0, 1):
        for phase in ("first", "second"):
            if read(which, phase, root_name) != root:
                raise ValueError("Root constants differ across boundary observations")
    thresholds = np.array(struct.unpack_from("<2f", root, root_offset + 100), dtype=np.float32)
    maximum = struct.unpack_from("<I", root, root_offset + 112)[0]
    if struct.unpack_from("<2I", root, root_offset + 152) != (48, 4) or maximum != 16384:
        raise ValueError("Unexpected clipmap/probe allocation")
    if not np.isfinite(thresholds).all() or not 0 < thresholds[1] < thresholds[0]:
        raise ValueError("Invalid distance thresholds")
    scale = np.array([struct.unpack_from("<f", root, root_offset + 240 + 16*c + 8)[0]
                      for c in range(4)], dtype=np.float32)
    bias = np.array([struct.unpack_from("<3f", root, root_offset + 432 + 16*c)
                     for c in range(4)], dtype=np.float32)
    view = read(1, "first", view_name)
    if read(1, "second", view_name) != view:
        raise ValueError("Observed view constants changed")
    tile = np.array(struct.unpack_from("<3f", view, view_offset + 960), dtype=np.float32)
    camera = np.array(struct.unpack_from("<3f", view, view_offset + 1104), dtype=np.float32)

    def costs(world):
        delta = np.float32(np.float32(camera - world) + np.float32(tile * np.float32(2097152)))
        squared = np.sum(np.float32(delta * delta), axis=1, dtype=np.float32)
        # Refuse near-threshold classifications; this CPU model is not a DXIL
        # contraction/transcendental emulator.
        magnitude = np.maximum(np.sum(np.square(np.abs(camera) + np.abs(world) +
                                                np.abs(tile * np.float32(2097152))), axis=1),
                               float(thresholds.max()))
        margin = np.min(np.abs(squared[:, None] - thresholds), axis=1)
        if not np.isfinite(squared).all() or np.any(margin <= 64 * np.finfo(np.float32).eps * magnitude):
            raise ValueError("Distance classification needs a GPU numerical reference")
        return np.where(squared < thresholds[1], 16, np.where(squared < thresholds[0], 4, 1))

    frames = {}
    for name in ("ProbeLastUsedFrame", "RWProbeLastTracedFrame"):
        raw = read(0, "first", name)
        if raw != read(0, "second", name) or len(raw) != maximum * 4:
            raise ValueError("Pre-boundary frame records are not stable complete arrays")
        frames[name] = np.frombuffer(raw, dtype="<u4")
    for phase in ("first", "second"):
        if (scalar(0, phase, "RWProbeTraceAllocator") != 0 or
                words(0, phase, "RWProbesToUpdateTraceCost").tolist() != [0, 0]):
            raise ValueError("Allocator/budget did not start at zero")

    rows = []
    reference_cells = None
    reference_frames = None
    for which in (0, 1):
        for phase in ("first", "second"):
            indirection = words(which, phase, "RadianceProbeIndirectionTexture")
            cells = np.flatnonzero(indirection != 0xffffffff)
            ids = indirection[cells]
            if np.any(ids >= maximum) or len(np.unique(ids)) != len(ids):
                raise ValueError("Invalid or duplicate active cache IDs")
            aligned_frames = np.column_stack([frames[n][ids] for n in frames])
            if reference_cells is None:
                reference_cells, reference_frames = cells, aligned_frames
            elif not np.array_equal(cells, reference_cells) or not np.array_equal(aligned_frames, reference_frames):
                raise ValueError("Grid-cell/frame reference differs across observations")
            bucket = scalar(which, phase, "MaxUpdateBucket")
            budget = scalar(which, phase, "MaxTracesFromMaxUpdateBucket")
            if not 1 <= bucket <= 15:
                raise ValueError("This check requires a positive boundary priority bucket")
            if bucket != scalar(0, "first", "MaxUpdateBucket") or budget != scalar(0, "first", "MaxTracesFromMaxUpdateBucket"):
                raise ValueError("Update bucket or boundary budget changed")
            if which == 0:
                continue
            clip = (cells % 192) // 48
            coordinates = np.column_stack((cells % 48, (cells // 192) % 48, cells // (192*48))).astype(np.float32)
            # The pinned shader's fast multiply/add pair contracts on the
            # observed GPU. Evaluate these bounded integer-grid/float32 inputs
            # in float64, then round once; every selected task position below
            # must still match the GPU's stored words exactly.
            world = (coordinates.astype(np.float64) * scale[clip, None].astype(np.float64) +
                     bias[clip].astype(np.float64)).astype(np.float32)
            weight = costs(world)
            used, traced = frames["ProbeLastUsedFrame"][ids], frames["RWProbeLastTracedFrame"][ids]
            age = np.where(used > traced, used.astype(np.int64) - traced.astype(np.int64), 1).astype(np.float32)
            priority = np.where(traced == 0, 0, np.floor(15 - np.clip(np.log2(age / (clip.astype(np.float32) + 1)), 0, 14))).astype(int)
            count = scalar(1, phase, "RWProbeTraceAllocator")
            data = words(1, phase, "RWProbeTraceData").reshape(-1, 4)
            if not 0 < count <= maximum or len(data) != maximum:
                raise ValueError("Invalid task prefix length")
            tasks = data[:count]
            task_ids = tasks[:, 3] & 0xffffff
            if len(np.unique(task_ids)) != count or not np.isin(task_ids, ids).all():
                raise ValueError("Task prefix references invalid/repeated probes")
            selected = np.isin(ids, task_ids)
            if not np.all(selected[priority < bucket]) or np.any(selected[priority > bucket]):
                raise ValueError("Observed selection is incompatible with priority buckets")
            task_cost = costs(tasks[:, :3].copy().view("<f4"))
            index_by_id = np.full(maximum, -1, dtype=np.int64)
            index_by_id[ids] = np.arange(len(ids))
            task_cells = index_by_id[task_ids]
            if (not np.array_equal(tasks[:, 3] >> 24, clip[task_cells]) or
                    not np.array_equal(task_cost, weight[task_cells]) or
                    not np.array_equal(tasks[:, :3], world[task_cells].view("<u4"))):
                raise ValueError("Task position/clipmap/cost disagrees with its active grid cell")
            observed_cost = words(1, phase, "RWProbesToUpdateTraceCost")
            total = int(weight[selected].sum())
            marginal = int(weight[selected & (priority == bucket)].sum())
            all_marginal = int(weight[priority == bucket].sum())
            omitted = weight[(~selected) & (priority == bucket)]
            if (observed_cost.tolist() != [total, all_marginal] or int(task_cost.sum()) != total or
                    marginal > budget or (len(omitted) and not np.any(omitted > budget - marginal))):
                raise ValueError("Observed costs cannot be an atomic budget prefix")
            rows.append({"phase": phase, "tasks": count, "mandatory_tasks": int((priority < bucket).sum()),
                         "mandatory_cost": int(weight[priority < bucket].sum()), "boundary_bucket": bucket,
                         "boundary_budget": budget, "boundary_candidates": int((priority == bucket).sum()),
                         "all_boundary_cost": all_marginal, "accepted_boundary_cost": marginal,
                         "accepted_total_cost": total,
                         "task_position_words_verified": count * 3,
                         "selected_cost_histogram": {str(c): int((task_cost == c).sum()) for c in (1, 4, 16)}})
    return {"status": "compatible_with_order_dependent_budget", "shader_sha256": SHADER,
            "resource_namespace": namespace,
            "allocation_inputs": {
                "scope": "Shader-used budget/distance inputs and frame references aligned by active grid cell; physical probe IDs excluded",
                "active_cells": len(reference_cells),
                "active_cells_sha256": hashlib.sha256(reference_cells.astype("<u4").tobytes()).hexdigest(),
                "aligned_frame_reference_sha256": hashlib.sha256(reference_frames.astype("<u4").tobytes()).hexdigest(),
                "frame_number": struct.unpack_from("<I", root, root_offset + 108)[0],
                "maximum_probes": maximum, "grid_resolution": 48, "clipmaps": 4,
                "distance_threshold_words": thresholds.view("<u4").tolist(),
                "clip_spacing_words": scale.view("<u4").tolist(),
                "clip_bias_words": bias.view("<u4").tolist(),
                "view_tile_words": tile.view("<u4").tolist(),
                "view_camera_words": camera.view("<u4").tolist(),
                "boundary_bucket": scalar(0, "first", "MaxUpdateBucket"),
                "boundary_budget": scalar(0, "first", "MaxTracesFromMaxUpdateBucket")},
            "scope": __doc__, "before_report": str(before_path.resolve()), "after_report": str(after_path.resolve()),
            "pipeline_report": str(pipeline_path.resolve()), "observations": rows,
            "frame_reference_scope": "Stable frame arrays from the independent pre-boundary pair; not a same-dispatch snapshot",
            "verified_resource_observations": len(verified)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("before", "after", "pipeline", "out"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--rdc-ids", action="store_true",
                        help="Use capture-local RenderDoc IDs and API boundaries; do not infer Flora identities")
    args = parser.parse_args()
    result = analyze(args.before, args.after, args.pipeline, args.rdc_ids)
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
