"""Execute the original postmesh branch against deterministic buffer fixtures."""
import argparse
import ast
import csv
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess
from types import SimpleNamespace as NS

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
source = (a.reference / 'standalone/rdc_worker.py').read_text('utf-8')
tree = ast.parse(source)
branch = next(n for n in ast.walk(tree) if isinstance(n, ast.If) and
              ast.unparse(n.test) == "job['action'] == 'postmesh'")
code = compile(ast.Module(body=branch.body, type_ignores=[]), 'original-postmesh-branch', 'exec')
namespace = dict(math=math)
plain = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'plain')
exec(compile(ast.Module(body=[plain], type_ignores=[]), 'original-plain', 'exec'), namespace)
rd = NS(ResourceId=NS(Null=lambda: 'ResourceId::0'), CompType=NS(Float=1),
        MeshDataStage=NS(VSOut=1), Topology=NS(TriangleList=5, TriangleStrip=6))
jobs, expected, labels = [], [], []

def case(label, positions, indices=None, width=2, base=0, topology=5, components=4,
         stride=16, raw=None, ib=None, comp_type=1, comp_width=4):
    vertices = raw if raw is not None else b''.join(struct.pack('<4f', *v) for v in positions)
    index_bytes = ib if ib is not None else (struct.pack('<' + str(len(indices)) +
                    {1:'B', 2:'H', 4:'I'}[width], *indices) if indices is not None else b'')
    metadata = dict(vertexResourceId='ResourceId::9', vertexByteOffset=0, vertexByteSize=len(vertices),
                    vertexByteStride=stride, indexResourceId='ResourceId::7' if indices is not None else 'ResourceId::0',
                    indexByteOffset=0, indexByteStride=width, baseVertex=base,
                    numIndices=len(indices) if indices is not None else len(positions), topology=topology,
                    format=dict(compType=comp_type, compByteWidth=comp_width, compCount=components),
                    unproject=False, nearPlane=0.1, farPlane=100.0, status='')
    mesh = NS(**dict(metadata, format=NS(**metadata['format'])))
    controller = NS(GetPostVSData=lambda *args: mesh,
                    GetBufferData=lambda resource, *args: vertices if resource == 'ResourceId::9' else index_bytes)
    target = a.out / label
    target.mkdir()
    result = {}
    env = dict(namespace, controller=controller, rd=rd, out=target, result=result, struct=struct, csv=csv,
               job=dict(instance=0, stage='VSOut'))
    try:
        exec(code, env)
        result = dict(ok=True, **{k: result[k] for k in ('vertex_count', 'index_count', 'face_count')},
                      csv=(target / 'post_vertices.csv').read_bytes().decode('utf-8'),
                      obj=(target / 'post_geometry.obj').read_bytes().decode('utf-8'))
    except Exception as error:
        result = dict(ok=False, error=str(error))
    jobs.append(dict(mesh=metadata, vertices=list(vertices), indices=list(index_bytes)))
    expected.append(result)
    labels.append(label)

points = [(0,0,0,1), (2,0,0,2), (0,3,0,3), (1,1,0,0), (-0.0,0,1,-1)]
for width in (1,2,4):
    for topology in (5,6):
        for base in (-2,0,1):
            case(f'indices-{width}-{topology}-{base}', points, [0,1,2,2,3,4,0,255,1], width, base, topology)
for topology in (1,2,3,5,6):
    case(f'nonindexed-{topology}', points, topology=topology, base=100)
for components in range(5):
    case(f'components-{components}', points, components=components)
case('special-floats', [(math.nan, math.inf, -math.inf,1), (-0.0,1e-8,1e20,0), (1,2,3,math.inf)])
case('empty', [])
case('remainder', points, raw=struct.pack('<4f', 1,2,3,1)+b'123')
case('zero-stride', points, stride=0)
case('unsupported-type', points, comp_type=2)
case('unsupported-width', points, comp_width=2)
case('truncated-position', points, stride=4, raw=b'12345678')
case('truncated-index', points, [0,1,2], ib=b'12')
case('oversized-index', points, [0,1,2], ib=b'12345678')
case('excess-components', points, components=5)
rng = random.Random(71833)
for i in range(40):
    values = [struct.unpack('<f', struct.pack('<I', rng.getrandbits(32)))[0] for _ in range(48)]
    case(f'float-bits-{i}', [tuple(values[j:j+4]) for j in range(0,48,4)])
input_path, output_path = a.out/'jobs.json', a.out/'native.json'
input_path.write_text(json.dumps(jobs), 'utf-8')
subprocess.run([str(a.exe.resolve()), '--probe', str(input_path.resolve()), str(output_path.resolve())], check=True)
actual = json.loads(output_path.read_text('utf-8'))
checks = []
for label, native, oracle in zip(labels, actual, expected, strict=True):
    valid = native['ok'] == oracle['ok'] and (not oracle['ok'] or native == oracle)
    checks.append(dict(name=label, passed=valid, native=native, oracle=oracle))
    print(label, 'PASS' if valid else 'FAIL')
report = dict(passed=all(c['passed'] for c in checks), cases=len(checks),
              source_sha256=hashlib.sha256(source.encode()).hexdigest(),
              oracle='Original rdc_worker.py postmesh AST body and plain function; mocked buffer transport only',
              checks=checks)
(a.out/'validation.json').write_text(json.dumps(report, indent=2), 'utf-8')
assert report['passed'], 'Native model differs from original Python branch'
