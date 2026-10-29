"""Compare contiguous mesh exports with a retained baseline model executable."""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--current', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    jobs = []
    def case(components, width, topology, base, values):
        indices = [0, 1, 2, (1 << (8 * width)) - 1, 2, 3, 0] if width else []
        raw = b''.join(struct.pack('<4f', *v) for v in values)
        ib = struct.pack('<' + {1: 'B', 2: 'H', 4: 'I'}[width] * len(indices), *indices) if width else b''
        jobs.append(dict(mesh=dict(format=dict(compType=1, compByteWidth=4, compCount=components),
                                   vertexByteStride=16, indexResourceId='ResourceId::7' if width else 'ResourceId::0',
                                   indexByteStride=width, baseVertex=base, topology=topology,
                                   numIndices=len(indices) if width else len(values)),
                         vertices=list(raw), indices=list(ib)))
    values = [(2, 4, 6, 2), (-0., 1, 2, 0), (math.nan, math.inf, -math.inf, 1), (1e-5, 1e20, 3, math.inf)]
    for components in range(1, 5):
        for width in (0, 1, 2, 4):
            for topology in (5, 6):
                for base in (-1, 0, 1):
                    case(components, width, topology, base, values)
    for components in range(5):
        case(components, 0, 5, 0, values)
    rng = random.Random(71833)
    for _ in range(32):
        words = [struct.unpack('<f', struct.pack('<I', rng.getrandbits(32)))[0] for _ in range(48)]
        case(4, 0, 5, 0, [words[i:i + 4] for i in range(0, 48, 4)])
    for field, value in (('vertexByteStride', 0), ('indexByteStride', 3), ('numIndices', 999)):
        job = copy.deepcopy(jobs[-1])
        job['mesh']['indexResourceId'] = 'ResourceId::7'
        job['indices'] = [0, 1, 2]
        job['mesh'][field] = value
        jobs.append(job)
    path = args.out / 'jobs.json'
    path.write_text(json.dumps(jobs), encoding='utf-8')
    results, identities = {}, {}
    for label, executable in (('baseline', args.baseline), ('current', args.current)):
        output = args.out / f'{label}.json'
        subprocess.run([str(executable.resolve()), '--probe', str(path.resolve()), str(output.resolve())],
                       check=True, timeout=30)
        results[label] = json.loads(output.read_text(encoding='utf-8'))
        identities[label] = dict(path=executable.as_posix(), sha256=hashlib.sha256(executable.read_bytes()).hexdigest())
    assert len(results['baseline']) == len(results['current']) == len(jobs), 'Missing probe cases'
    differences = [i for i, (before, after) in enumerate(zip(results['baseline'], results['current'], strict=True)) if before != after]
    report = dict(schema=1, status='different' if differences else 'equal', cases=len(jobs), differences=differences,
                  executables=identities, scope='contiguous_model_compatibility', full_workflow_parity=False)
    (args.out / 'validation.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report))
    return int(bool(differences))


if __name__ == '__main__':
    raise SystemExit(main())
