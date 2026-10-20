"""Offline Hellblade2 screen-probe diagnosis; Python/numpy is not an app dependency.

Consumes rdc-repeat-selection reports and decoded, shader-verified constants.
Only use --texture for a verified square per-probe block layout (for example
8x8 radiance or 1x1 depth). It does not infer shader layouts or validate GPA
fidelity. Resource identities remain strings; unused allocation tails are excluded.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np


def compare(report_path, constants_path, positions, counter, textures):
    report = json.loads(report_path.read_text(encoding="utf-8"))
    constants = json.loads(constants_path.read_text(encoding="utf-8"))
    if not report.get("repeat_only") or report.get("history_requested"):
        raise ValueError("Expected a repeat-only probe without PixelHistory")
    if report.get("between_event") is not None:
        raise ValueError("A restoration round trip is not a paired computation")
    resources = {item["creation"]: (i, item) for i, item in enumerate(report["resources"])}
    if len(resources) != len(report["resources"]):
        raise ValueError("Duplicate resource lifetimes in report")
    phases = ("first", "second")

    def read(phase, creation):
        index, description = resources[creation]
        matches = [item for item in report["readback_files"]
                   if item["phase"] == phase and item["creation"] == creation]
        if len(matches) != 1:
            raise ValueError(f"Expected one {phase} readback for {creation}")
        path = Path(matches[0]["file"])
        if not path.is_absolute():
            path = report_path.parent / path
        size = path.stat().st_size
        observation = report[phase][index]
        if not 0 < size <= 32 * 1024 * 1024 or size != int(observation["bytes"]):
            raise ValueError(f"Invalid bounded readback size: {path}")
        raw = path.read_bytes()
        if hashlib.sha256(raw).hexdigest() != observation["storage_sha256"]:
            raise ValueError(f"Readback digest mismatch: {path}")
        return description, raw

    def words(phase, creation):
        description, raw = read(phase, creation)
        if description["kind"] != "buffer" or len(raw) % 4:
            raise ValueError("Expected complete uint32 buffer storage")
        if description["byte_offset"] != "0" or int(description["byte_length"]) != len(raw):
            raise ValueError("Expected a full bounded buffer observation")
        return np.frombuffer(raw, dtype="<u4")

    atlas_width, atlas_height = constants["atlas_buffer_size"]
    uniform = constants["uniform_probes"][0]
    maximum = constants["max_adaptive"][0]
    if not (atlas_width > 0 and atlas_height > 0 and
            0 <= uniform <= atlas_width * atlas_height <= 1_000_000 and
            0 <= maximum <= 65536):
        raise ValueError("Invalid bounded probe dimensions")
    counters = [words(phase, counter) for phase in phases]
    if any(len(value) != 1 for value in counters):
        raise ValueError("Expected a single uint32 adaptive counter")
    counts = [int(value[0]) for value in counters]
    if counts[0] != counts[1] or not 0 <= counts[0] <= maximum:
        raise ValueError("Adaptive counts differ or exceed the verified capacity")
    count = counts[0]
    if uniform + count > atlas_width * atlas_height:
        raise ValueError("Active probes exceed the atlas")
    packed = [words(phase, positions) for phase in phases]
    if any(len(value) < count for value in packed):
        raise ValueError("Adaptive position buffer is truncated")
    packed = [value[:count] for value in packed]
    if any(len(np.unique(value)) != count for value in packed):
        raise ValueError("Active screen positions are not unique")
    if not np.array_equal(np.sort(packed[0]), np.sort(packed[1])):
        raise ValueError("Active screen-position sets differ; cannot align by permutation")
    second_index = {int(position): i for i, position in enumerate(packed[1])}
    order = np.concatenate((np.arange(uniform, dtype=np.int64),
                            np.array([uniform + second_index[int(p)] for p in packed[0]],
                                     dtype=np.int64)))
    rows = []
    for creation in textures:
        blocks = []
        raw_equal = []
        for phase in phases:
            description, raw = read(phase, creation)
            if (description["kind"] != "texture" or description["array_size"] != 1 or
                    description["mips"] != 1 or description["samples"] > 1 or
                    description["selected_subresource"] != {"mip": 0, "layer": 0, "sample": 0}):
                raise ValueError("Expected a single-mip, single-layer, single-sample texture")
            width, height = description["width"], description["height"]
            if (width <= 0 or height <= 0 or width % atlas_width or height % atlas_height or
                    width // atlas_width != height // atlas_height or len(raw) % (width * height)):
                raise ValueError("Texture does not fit the specified square probe-block layout")
            block = width // atlas_width
            stride = len(raw) // (width * height)
            if stride not in (1, 2, 4, 8, 12, 16):
                raise ValueError("Unsupported per-texel storage stride")
            blocks.append(np.frombuffer(raw, np.uint8)
                          .reshape(atlas_height, block, atlas_width, block, stride)
                          .transpose(0, 2, 1, 3, 4)
                          .reshape(atlas_width * atlas_height, block * block, stride))
            raw_equal.append(hashlib.sha256(raw).hexdigest())
        different = (blocks[0][:uniform + count] != blocks[1][order]).any(axis=2)
        probes = different.any(axis=1)

        def example(start, stop):
            if not np.any(different[start:stop]):
                return None
            probe, texel = divmod(int(np.argmax(different[start:stop])), different.shape[1])
            probe += start
            second = int(order[probe])
            coordinate = lambda index: [(index % atlas_width) * block + texel % block,
                                        (index // atlas_width) * block + texel // block]
            position = int(packed[0][probe - uniform]) if probe >= uniform else None
            return {
                "first_probe_index": probe, "second_probe_index": second,
                "first_texel": coordinate(probe), "second_texel": coordinate(second),
                "adaptive_screen_position": None if position is None else [position & 65535, position >> 16],
                "before_bytes": blocks[0][probe, texel].tolist(),
                "after_bytes": blocks[1][second, texel].tolist(),
            }

        rows.append({
            "creation": creation,
            "probe_block_extent": [block, block],
            "bytes_per_texel": stride,
            "raw_storage_equal": raw_equal[0] == raw_equal[1],
            "changed_uniform_probe_blocks": int(probes[:uniform].sum()),
            "changed_adaptive_probe_blocks": int(probes[uniform:].sum()),
            "changed_active_texels": int(different.sum()),
            "first_changed_uniform_texel": example(0, uniform),
            "first_changed_adaptive_texel": example(uniform, uniform + count),
        })
    return {
        "status": "compared",
        "scope": "Position-aligned active probe storage only; layout supplied from shader evidence. "
                 "No original-GPA equivalence or root-cause claim.",
        "report": str(report_path.resolve()), "constants": str(constants_path.resolve()),
        "event": report["event"], "positions_creation": positions, "counter_creation": counter,
        "adaptive_count": count, "uniform_count": uniform,
        "active_screen_positions_equal": True, "textures": rows,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("constants", type=Path)
    parser.add_argument("--positions", required=True)
    parser.add_argument("--counter", required=True)
    parser.add_argument("--texture", action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    result = compare(args.report, args.constants, args.positions, args.counter, args.texture)
    # Never replace earlier evidence.
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
        output.write("\n")


if __name__ == "__main__":
    main()
