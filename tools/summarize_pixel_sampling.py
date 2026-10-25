"""Validate independent implicit sampling matrices and report coverage-dependent results."""

import argparse
import itertools
import json
from pathlib import Path
import re
import struct

from summarize_dxbc_execution import identity


def bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def floating(value):
    return struct.unpack("<f", struct.pack("<I", value))[0]


def summarize(root):
    names = ["point", "linear", "anisotropic", "bias", "minimum", "maximum", "resource-minimum",
             "view-mips", "border", "wrap", "mirror", "mirror-once", "null-resource", "null-sampler"]
    names += ["comparison-function"] * 7
    functions = [4] * 14 + [1, 2, 3, 5, 6, 7, 8]
    reports, shaders, coverage_differences = [], {}, []
    counts = dict(cases=0, defined_active_vectors=0, undefined_active_vectors=0,
                  untouched_vectors=0, analytical_point_vectors=0, zero_resource_vectors=0)
    for vendor in (0, 4318, 32902):
        path = root / str(vendor) / "results.json"
        report = json.loads(path.read_text(encoding="utf-8"))
        if (report["scope"] != "independent_pixel_sampling" or report["schema"] != 1
                or report["vendor"] != vendor or report["adapter_vendor"] != (vendor or 5140)
                or report["unexpected_validation_errors"] or not report["adapter_luid"]):
            raise ValueError("Invalid implicit sampling identity")
        reports.append(identity(path))
        expected = {(op, offset, config, dataset, coverage)
                    for op in ("sample", "sample_b", "sample_c")
                    for offset, config, dataset, coverage in itertools.product(
                        range(2), range(21 if op == "sample_c" else 14), range(8), (15, 3, 5, 1))}
        seen, cases = set(), {}
        for item in report["cases"]:
            op, offset, config, dataset, coverage = key = tuple(item[k] for k in
                ("operation", "offset", "configuration", "dataset", "coverage"))
            if key not in expected or key in seen:
                raise ValueError("Unexpected or duplicate implicit sampling case")
            seen.add(key)
            cases[key] = item
            comparison = op == "sample_c"
            flow = "uniform" if dataset < 4 else "discard" if dataset < 6 else "reconverge" if dataset == 6 else "loop"
            discarded = 1 if dataset == 4 else 5 if dataset == 5 else 0
            shader_name = f"{op}-{flow}-{offset}.dxbc"
            if (item["flow"] != flow or item["name"] != names[config] or item["shader"] != shader_name
                    or item["discarded"] != discarded or item["resource_bound"] != (config != 12)
                    or item["comparison"] != functions[config] or item["first_mip"] != (1 if config == 7 else 0)
                    or item["mip_count"] != (2 if config == 7 else 4)
                    or item["resource_min_lod"] != (1.25 if config == 6 else 0)):
                raise ValueError("Implicit sampling state matrix changed")
            sampler = item["sampler_descriptor"]
            descriptor = [([0,21,85][config] if config < 3 else 0) | (128 if comparison else 0)]
            address = {8:4,9:1,10:2,11:5}.get(config,3)
            descriptor += [address]*3 + [bits(2 if config == 3 else 0),8,functions[config],bits(.375),0,0,0,
                                        bits(1.5) if config == 4 else 0xff7fffff,
                                        bits(.5) if config == 5 else 0x7f7fffff]
            if sampler != (None if config == 13 else descriptor):
                raise ValueError("Implicit sampler descriptor changed")
            scale = (0,.03125,.5,.25)[min(dataset,3)]
            inputs = [[bits(.375+(lane%2)*scale), bits(.375+(lane//2)*scale+(1 if dataset==3 and lane==3 else 0)),
                       bits((.125,.5,1,2)[lane] if comparison else (-2,-.5,.5,2)[lane]),0] for lane in range(4)]
            if comparison and dataset == 6:
                inputs[2][2], inputs[3][2] = 0x7f800000, 0x7fc12345
            if item["inputs"] != inputs or len(item["output"]) != 4:
                raise ValueError("Implicit sampling input matrix changed")
            undefined = comparison and config == 13
            if item["status"] != ("undefined_missing_comparison_sampler" if undefined else "observed"):
                raise ValueError("Undefined implicit comparison result was accepted")
            if undefined:
                if not item["diagnostics"] or any(d["id"] != 390 for d in item["diagnostics"]):
                    raise ValueError("Missing precise comparison sampler diagnostic")
            elif item["diagnostics"]:
                raise ValueError("Unexpected implicit sampling validation error")
            shader = path.parent / shader_name
            digest = identity(shader)["sha256"]
            if shader_name in shaders and shaders[shader_name] != digest:
                raise ValueError("Implicit sampling shader differs between adapters")
            if shader_name not in shaders:
                assembly = shader.with_suffix(".asm.txt").read_text(encoding="utf-8")
                token = op + ("_aoffimmi_indexable" if offset else "_indexable")
                if not re.search("^" + token + r"\(", assembly, re.MULTILINE) or "ps_5_0" not in assembly:
                    raise ValueError("Compiler changed the implicit sampling opcode")
                if flow == "discard" and "discard" not in assembly:
                    raise ValueError("Compiler removed sampling discard")
                if flow == "loop" and "endloop" not in assembly:
                    raise ValueError("Compiler removed sampling loop")
                shaders[shader_name] = digest
            for lane, output in enumerate(item["output"]):
                if len(output) != 4 or any(type(b) is not int or not 0 <= b <= 0xffffffff for b in output):
                    raise ValueError("Malformed sampling output bits")
                if not (coverage & ~discarded) & (1 << lane):
                    if output != [0xdeadbeef]*4:
                        raise ValueError("Uncovered or discarded pixel changed the target")
                    counts["untouched_vectors"] += 1
                    continue
                counts["undefined_active_vectors" if undefined else "defined_active_vectors"] += 1
                if undefined:
                    continue
                if len(set(output)) != 1:
                    raise ValueError("Sample result red replication was lost")
                if config == 12:
                    if output != [0]*4:
                        raise ValueError("Unbound implicit resource did not return zero")
                    counts["zero_resource_vectors"] += 1
                if comparison and not 0 <= floating(output[0]) <= 1:
                    raise ValueError("Invalid comparison filtering range")
                if dataset == 0 and (config == 0 or config >= 14):
                    depth = .25+(3-offset)/8+(3+offset)/64
                    expected_value = depth
                    if comparison:
                        reference = floating(inputs[lane][2])
                        values = [False,reference < depth,reference == depth,reference <= depth,
                                  reference > depth,reference != depth,reference >= depth,True]
                        expected_value = float(values[functions[config]-1])
                    if output != [bits(expected_value)]*4:
                        raise ValueError("Implicit point result disagrees with independent arithmetic")
                    counts["analytical_point_vectors"] += 1
            counts["cases"] += 1
        if seen != expected:
            raise ValueError("Incomplete implicit sampling matrix")
        for key, item in cases.items():
            op, offset, config, dataset, coverage = key
            if coverage == 15 or item["status"] != "observed":
                continue
            full = cases[(op,offset,config,dataset,15)]
            for lane in range(4):
                if coverage & ~item["discarded"] & (1 << lane) and item["output"][lane] != full["output"][lane]:
                    coverage_differences.append(dict(vendor=vendor,operation=op,offset=offset,configuration=config,
                        dataset=dataset,coverage=coverage,lane=lane,full=full["output"][lane],partial=item["output"][lane]))
    return dict(schema=1,scope="independent_implicit_pixel_sampling",reports=reports,shader_sha256=shaders,
                counts=counts,coverage_differences=coverage_differences,
                captured_invocations_validated=False,complete_release_acceptance=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "binary", "out"):
        parser.add_argument("--" + name,type=Path,required=True)
    args = parser.parse_args()
    result = summarize(args.root)
    result["binary"] = identity(args.binary)
    result["source"] = identity(Path("tools/native/pixel_sampling_probe.cpp"))
    args.out.write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(result["counts"]))
    print("Coverage differences:",len(result["coverage_differences"]))


if __name__ == "__main__":
    main()
