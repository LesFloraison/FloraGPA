"""Independent native coverage execution against the preserved Python GPU engine.

Fixtures record initialization as API commands. Compare reports, exact decoded
image bytes and every initialized output, including hidden UAV counters.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--reference', type=Path, required=True)
    p.add_argument('--probe', type=Path, required=True)
    p.add_argument('--qt-bin', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--suite', choices=('core','extended','integration','real','all'), default='all')
    p.add_argument('--frames', type=Path)
    args = p.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sys.path[:0] = [str(args.reference/'standalone'), str(args.reference/'tools')]
    from coverage import export_coverage
    from frame import Frame
    from engine import Engine
    from devices import create_device
    from image import read_png
    from texture_readback import read_storage
    from coverage_isolated import counter_value
    from validate_coverage_bindings import fixture as binding_fixture
    from validate_coverage_isolated import fixture as isolated_fixture
    from validate_coverage_viewport import fixture as viewport_fixture
    from validate_class_linkage import clone
    from msaa import inspect_msaa
    from validate_coverage_dimensions import fixture as dimension_fixture
    from validate_coverage_buffer_rtv import fixture as buffer_fixture
    from validate_coverage_viewport_arrays import fixture as array_fixture
    from validate_pre_raster_coverage import make as preraster_fixture
    from validate_stream_output import fixture as so_fixture
    from validate_so_coverage import viewport as so_viewport
    from validate_predication import fixture as predicate_fixture
    from validate_buffer_edits import experiment
    from validate_texture_outputs import patch

    def seeded(path, name):
        with Frame(path) as f:
            resources = [f.resource(en.id) for en in f.entries.values() if en.category == 5 and en.type == 0x85]
        def seed(x):
            eid = 40
            for resource in resources:
                desc = resource['desc']
                for layer in range(desc[3]):
                    for mip in range(desc[2]):
                        view = 10000 + eid
                        if desc[4] == 44:
                            fields = [45, 6, 0, layer, 1, 0] if desc[5] > 1 else [45, 4, 0, mip, layer, 1]
                            x.add(view, 5, 0x8e, bytes(16)+struct.pack('<Q6I', resource['id'], *fields))
                            x.add(eid, 7, 0x31, struct.pack('<QQQIfB', 0, 1, view, 3, .75, 3))
                        else:
                            fields = [28, 7, layer, 1, 0] if desc[5] > 1 else [28, 5, mip, layer, 1]
                            x.view(view, 'rtv', resource['id'], fields)
                            x.add(eid, 7, 0x32, struct.pack('<QQQB4f', 0, 1, view, 1, 13/255, 17/255, 19/255, 23/255))
                        eid += 1
        return clone(path, out/(name+'.gpa_frame'), lambda en, raw: raw, seed)

    fixtures = []
    for name, kw in [
        ('sparse', dict(color_slots=(0,3), uav_slots=(7,))),
        ('full', dict(color_slots=tuple(range(8)), uav_slots=())),
        ('depth-u0', dict()),
        ('depth-null', dict(color_slots=(), uav_slots=(3,), null_ps=True)),
        ('array', dict(color_slots=(0,3), uav_slots=(), array=True, mip=1)),
        ('msaa', dict(color_slots=(0,3), uav_slots=(), samples=4)),
    ]:
        path = seeded(binding_fixture(out/(name+'-source.gpa_frame'), **kw), name)
        fixtures.append((name, path, dict(layer=2) if kw.get('array') else {}))
    for name, kw in [('dual', dict(kind='dual')), ('isolated-uav', dict(kind='uav')), ('atoc', dict(kind='dual', samples=4, atoc=True, returns=True))]:
        fixtures.append((name, seeded(isolated_fixture(out/(name+'-source.gpa_frame'), **kw), name), {}))
    for name, kw in [('viewport', {}), ('viewport-eight', dict(slots=tuple(range(8)))), ('viewport-null', dict(kind='null', slots=())), ('viewport-negative', dict(negative=True)), ('viewport-coverage', dict(coverage=True, slots=(1,)))]:
        fixtures.append((name, viewport_fixture(out/(name+'.gpa_frame'), **kw), {}))
    if args.suite in ('extended','integration','real'): fixtures.clear()
    if args.suite in ('extended','all'):
        for name, kw in [
            ('1d', dict(array=False, route=0, uavs=0)), ('1d-array', dict(uavs=0)),
            ('1d-full', dict(slots=tuple(range(8)), uavs=0)), ('1d-depth', dict(slots=(), depth=True)),
            ('1d-readonly', dict(slots=(), depth=True, readonly=True)),
            ('1d-depth-eight', dict(slots=(), depth=True, uavs=8)),
            ('3d', dict(dim='3d')), ('3d-all', dict(dim='3d', all_w=True)),
            ('3d-full', dict(dim='3d', slots=tuple(range(8)), uavs=0)), ('3d-uint', dict(dim='3d', fmt=42))]:
            fixtures.append((name, dimension_fixture(out/(name+'.gpa_frame'), **kw), dict(event=500, layer=2 if kw.get('array',True) else 0)))
        for fmt in (28, 42, 41, 2, 49, 61):
            name = f'buffer-{fmt}'
            fixtures.append((name, buffer_fixture(out/(name+'.gpa_frame'), fmt=fmt), dict(event=500)))
        for producer in ('gs','vs','ds'):
            for input_index in (False,True):
                name = f'viewport-array-{producer}-{int(input_index)}'
                path = array_fixture(out/(name+'.gpa_frame'), producer=producer, input_index=input_index)
                for layer in (None,1,2048,0): fixtures.append((name+f'-{layer}',path,dict(layer=layer)))
        for stage in ('vs','hs','ds','gs'):
            for slot in (0,63):
                for viewport in (False,True):
                    name = f'preraster-{stage}-{slot}-{int(viewport)}'
                    path = out/(name+'.gpa_frame')
                    preraster_fixture(path,stage,slot,True,viewport)
                    fixtures.append((name,path,{}))
    if args.suite in ('integration','all'):
        for full in (False,True):
            name = f'so-{int(full)}'
            path = out/(name+'.gpa_frame')
            so_fixture(path,append=True,full_targets=full)
            for event in (100,150,200): fixtures.append((name+f'-{event}',path,dict(event=event)))
            viewport = so_viewport(path,out/(name+'-viewport.gpa_frame'))
            for event in (100,150): fixtures.append((name+f'-viewport-{event}',viewport,dict(event=event)))
        for visible in (False,True):
            for predicate in (0,1):
                name = f'predicate-{int(visible)}-{predicate}'
                path = predicate_fixture(out/(name+'.gpa_frame'),visible=visible,value=predicate)
                fixtures.append((name,path,dict(event=200)))
        for input_index in (False,True):
            name = f'dynamic-array-{int(input_index)}'
            path = array_fixture(out/(name+'.gpa_frame'),dynamic=True,input_index=input_index)
            fixtures.append((name,path,dict(layer=1)))
        path = seeded(isolated_fixture(out/'alias-source.gpa_frame',alias=True), 'alias')
        fixtures.append(('alias-input',path,dict(operations=[patch(1000,1,0,bytes([200,0,0,255])*6,kind='texture_input')])))
        fixtures.append(('alias-disabled',path,dict(operations=[dict(kind='enabled',event=100,value=False)])))
        path = seeded(binding_fixture(out/'targets-source.gpa_frame',color_slots=(0,3),uav_slots=(7,)), 'targets')
        for target in ('rt0','rt3','depth','rt1'):
            fixtures.append(('select-'+target,path,dict(target=target)))
        fixtures.append(('target-output-edit',path,dict(operations=[patch(1400,0,0,bytes([200,0,0,255])*70)])))
    if args.suite == 'real':
        if args.frames is None: p.error('--suite real requires --frames')
        for stem in ('GF2_Exilium_2026_03_03__00_19_35','bf1_2026_01_21__16_53_05'):
            path = (args.frames/(stem+'.gpa_frame')).resolve()
            with Frame(path) as frame:
                draws = sorted(e.id for e in frame.entries.values() if e.category == 7 and 0x37 <= e.type <= 0x3d)
            for event in (draws[len(draws)//2],draws[-1]):
                fixtures.append((f'{stem}-{event}',path,dict(event=event,real=True)))

    jobs, expected, names = [], [], []
    def sha(data): return hashlib.sha256(data).hexdigest()
    for name, path, extra in fixtures:
        real = extra.get('real',False)
        for warp in ((False,) if real else (False, True)):
            for mode, depth in ([('fragment',True),('geometry',True)] if real else [('fragment', True), ('fragment', False), ('geometry', True), ('geometry', False)]):
                label = f'{name}-{int(warp)}-{mode}-{int(depth)}'
                settings = dict(extra)
                settings.pop('real',None)
                event = settings.pop('event',100)
                operations = settings.pop('operations',None)
                job = dict(capture=str(path), event=event, warp=warp, mode=mode, depth_test=depth, **settings)
                with Frame(path) as frame:
                    job['buffers'] = [e.id for e in frame.entries.values() if e.category == 5 and e.type == 0x83]
                    job['textures'] = [e.id for e in frame.entries.values() if e.category == 5 and (e.type in (0x84,0x86) or (e.type == 0x85 and frame.resource(e.id)['desc'][5] == 1))]
                    job['msaa'] = [dict(id=e.id,samples=frame.resource(e.id)['desc'][5],format=45 if frame.resource(e.id)['desc'][4] == 44 else frame.resource(e.id)['desc'][4]) for e in frame.entries.values() if e.category == 5 and e.type == 0x85 and frame.resource(e.id)['desc'][5] > 1]
                    job['counters'] = [e.id for e in frame.entries.values() if e.category == 5 and e.type == 0x8f and struct.unpack_from('<I', frame.payload(e.id), 40)[0] & 4]
                    if real:
                        for key in ('buffers','textures','counters','msaa'): job[key] = []
                    device = create_device('warp' if warp else 'hardware')
                    try:
                        project = None
                        if operations is not None:
                            job['experiment'] = str(out/(label+'-experiment.json'))
                            project = experiment(frame,Path(job['experiment']),operations)
                        engine = Engine(frame, device,experiment=project)
                        directory = out/(label+'-python')
                        report = export_coverage(engine, event, directory, depth_test=depth, mode=mode, **settings)
                        value = dict(report=report, storage={}, counters={}, msaa={}, so_history=engine.so_count_history)
                        if depth or mode == 'geometry': value['preserves_original'] = True
                        for key, file in [('mask','coverage.png'), ('after','after_draw.png'), ('overlay','overlay.png')]:
                            w, h, data = read_png(directory/file)
                            value[key] = sha(data)
                            value['size'] = [w,h]
                        for resource in job['buffers']: value['storage'][str(resource)] = sha(engine.read_buffer(resource))
                        for resource in job['textures']: value['storage'][str(resource)] = sha(read_storage(device, engine.resources.get(resource), frame.resource(resource)))
                        for view in job['counters']: value['counters'][str(view)] = counter_value(device, engine.resources.get(view))
                        for resource in job['msaa']:
                            rid = resource['id']
                            value['msaa'][str(rid)] = [sha(inspect_msaa(device,engine.resources.get(rid),frame.resource(rid),s,typed_format=resource['format'])[1]) for s in range(resource['samples'])]
                    except Exception as error:
                        value = dict(error=str(error))
                    finally: device.close()
                jobs.append(job)
                expected.append(value)
                names.append(label)
                print('reference', label, value.get('error','ok'), flush=True)
    (out/'jobs.json').write_text(json.dumps(jobs, indent=2))
    (out/'expected.json').write_text(json.dumps(expected, indent=2))
    env = dict(os.environ)
    env['PATH'] = str(args.qt_bin) + os.pathsep + env.get('PATH','')
    subprocess.run([str(args.probe.resolve()), '--probe', str(out/'jobs.json'), str(out/'actual.json')], env=env, check=True, timeout=600)
    actual = json.loads((out/'actual.json').read_text())
    checks = []
    for name, wanted, got in zip(names, expected, actual, strict=True):
        if 'error' in wanted:
            checks.append(dict(name=name+' rejection', passed='error' in got, expected=wanted, actual=got))
        else:
            for key in wanted:
                checks.append(dict(name=name+' '+key, passed=wanted[key]==got.get(key), **({} if wanted[key]==got.get(key) else dict(expected=wanted[key], actual=got.get(key), error=got.get('error')))))
    result = dict(passed=all(c['passed'] for c in checks), cases=len(jobs), checks=checks)
    (out/'validation.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(dict(passed=result['passed'], cases=len(jobs), checks=len(checks), failures=[c for c in checks if not c['passed']]), indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
