"""Compare original GPA Plugin API exports with retained Flora GF2 inspections.

Development-only, CPU-only validation. Requires Windows D3DDisassemble, never
loads GPA DLLs or substitutes metadata checks for actual replay pixel checks.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path

from audit_capture_gf2_evidence import digest, read, require


def actions(document, original):
    source = document['calls' if original else 'commands']
    return [row for row in source if row['name'].startswith(('Draw', 'Dispatch'))]


def signature(row, original):
    args = [a['value'] for a in row['arguments']] if original else row['parameters']
    return row['name'], args


def match_actions(original, flora):
    left, right = actions(original, True), actions(flora, False)
    require(left and len(left) == len(right), 'Draw/Dispatch counts differ')
    require(len({x['id'] for x in left}) == len(left) and len({x['id'] for x in right}) == len(right),
            'Duplicate event identity')
    require(all(signature(a, True) == signature(b, False) for a, b in zip(left, right)),
            'Ordered Draw/Dispatch names or arguments differ')
    return {int(b['id']): a for a, b in zip(left, right)}


def clean_assembly(text):
    return text.rstrip('\0').replace('\r\n', '\n')


def disassemble(data):
    require(data[:4] == b'DXBC', 'Expected DXBC shader')
    dll = c.WinDLL(str(Path(os.environ['SystemRoot'])/'System32/d3dcompiler_47.dll'))
    fn = dll.D3DDisassemble
    fn.argtypes = [c.c_void_p, c.c_size_t, c.c_uint, c.c_char_p, c.POINTER(c.c_void_p)]
    fn.restype = c.c_long
    storage, blob = c.create_string_buffer(data), c.c_void_p()
    result = fn(storage, len(data), 0, None, c.byref(blob))
    require(result >= 0 and blob.value, 'D3DDisassemble failed')
    table = c.cast(blob, c.POINTER(c.POINTER(c.c_void_p))).contents
    try:
        pointer = c.WINFUNCTYPE(c.c_void_p, c.c_void_p)(table[3])(blob)
        size = c.WINFUNCTYPE(c.c_size_t, c.c_void_p)(table[4])(blob)
        return clean_assembly(c.string_at(pointer, size).decode('utf-8'))
    finally:
        c.WINFUNCTYPE(c.c_ulong, c.c_void_p)(table[2])(blob)


def enum(prefix, value, names, first=1):
    require(first <= value < first+len(names) and names[value-first] is not None, 'Unverified D3D enum')
    return 'D3D11_'+prefix+'_'+names[value-first]


def graphics_state(fields):
    """Project native getters onto precisely the state fields exposed by GPA."""
    def value(name):
        row = fields[name]
        require(row['known'], 'Unknown native field: '+name)
        return row['value']

    raster, depth, blend = [value(k+'.descriptor') for k in ('rasterizer', 'depth_state', 'blend')]
    result = {'Blending': {'ALPHA TO COVERAGE ENABLE': blend['alpha_to_coverage'],
                          'INDEPENDENT BLEND ENABLE': blend['independent_blend'],
                          'FACTOR': [value('blend_factor.'+str(i)) for i in range(4)]},
              'Depth': {'ENABLE': depth['depth_enable'],
                        'WRITE MASK': enum('DEPTH_WRITE_MASK', depth['depth_write_mask'], ['ZERO','ALL'], 0),
                        'FUNC': enum('COMPARISON', depth['depth_func'], ['NEVER','LESS','EQUAL','LESS_EQUAL','GREATER','NOT_EQUAL','GREATER_EQUAL','ALWAYS'])},
              'Stencil': {'ENABLE': depth['stencil_enable'], 'READ MASK': depth['stencil_read_mask'],
                          'WRITE MASK': depth['stencil_write_mask'], 'STENCIL REF': value('stencil_ref')},
              'Depth-Stencil Operations': {}, 'Rasterizer': {}}
    for side in ('front', 'back'):
        for source, label in (('fail_op','FAIL OP'), ('depth_fail_op','DEPTH FAIL OP'), ('pass_op','PASS OP'), ('func','FUNC')):
            number = depth[side+'_face'][source]
            names = ['NEVER','LESS','EQUAL','LESS_EQUAL','GREATER','NOT_EQUAL','GREATER_EQUAL','ALWAYS'] if source == 'func' else ['KEEP','ZERO','REPLACE','INCR_SAT','DECR_SAT','INVERT','INCR','DECR']
            result['Depth-Stencil Operations'][side.upper()+' STENCIL '+label] = enum('COMPARISON' if source == 'func' else 'STENCIL_OP', number, names)
    raster_labels = {'front_counter_clockwise':'FRONT COUNTER CLOCKWISE', 'depth_bias':'DEPTH BIAS',
                     'depth_bias_clamp':'DEPTH BIAS CLAMP', 'slope_scaled_depth_bias':'SLOPE SCALED DEPTH BIAS',
                     'depth_clip_enable':'DEPTH CLIP ENABLE', 'scissor_enable':'SCISSOR ENABLE',
                     'multisample_enable':'MULTISAMPLING ENABLE', 'antialiased_line_enable':'ANTIALIASED LINE ENABLE',
                     'forced_sample_count':'FORCED SAMPLE COUNT'}
    result['Rasterizer'] = {label:raster[key] for key,label in raster_labels.items()}
    result['Rasterizer'].update({'FILL MODE':enum('FILL', raster['fill_mode'], ['WIREFRAME','SOLID'], 2),
        'CULL MODE':enum('CULL', raster['cull_mode'], ['NONE','FRONT','BACK']),
        'CONSERVATIVE RASTERIZER':enum('CONSERVATIVE_RASTERIZATION_MODE', raster['conservative_raster'], ['OFF','ON'], 0)})
    blend_names = ['ZERO','ONE','SRC_COLOR','INV_SRC_COLOR','SRC_ALPHA','INV_SRC_ALPHA','DEST_ALPHA','INV_DEST_ALPHA','DEST_COLOR','INV_DEST_COLOR','SRC_ALPHA_SAT',None,None,'BLEND_FACTOR','INV_BLEND_FACTOR','SRC1_COLOR','INV_SRC1_COLOR','SRC1_ALPHA','INV_SRC1_ALPHA']
    for i in range(8):
        target = blend['targets'][str(i)]
        row = {'ENABLE':target['blend_enable'], 'RENDER TARGET WRITE MASK':target['write_mask'], 'LOGIC OP ENABLE':target['logic_op_enable'],
               'LOGIC OP':enum('LOGIC_OP', target['logic_op'], ['CLEAR','SET','COPY','COPY_INVERTED','NOOP','INVERT','AND','NAND','OR','NOR','XOR','EQUIV','AND_REVERSE','AND_INVERTED','OR_REVERSE','OR_INVERTED'], 0)}
        for name,label in (('src_blend','SOURCE BLEND'),('dest_blend','DESTINATION BLEND'),('src_blend_alpha','SOURCE BLEND ALPHA'),('dest_blend_alpha','DESTINATION BLEND ALPHA')):
            row[label] = enum('BLEND', target[name], blend_names)
        for name,label in (('blend_op','BLEND OP'),('blend_op_alpha','BLEND OP ALPHA')):
            row[label] = enum('BLEND_OP', target[name], ['ADD','SUBTRACT','REV_SUBTRACT','MIN','MAX'])
        result[f'RT{i} Blending'] = row
    for kind,key,labels in (('Viewport','viewports',['TOP LEFT X','TOP LEFT Y','WIDTH','HEIGHT','MIN DEPTH','MAX DEPTH']),
                            ('Scissor Rect','scissors',['LEFT','TOP','RIGHT','BOTTOM'])):
        for i, row in enumerate(value(key)):
            require(len(row) == len(labels), 'Invalid native rectangle')
            result[f'{kind} {i}'] = dict(zip(labels,row))
    return result


def group_ids(nodes):
    result = set()
    for node in nodes:
        if node['type'] == 'group':
            result.update(group_ids(node['children']))
        elif node['type'] == 'call':
            result.add(node['eid'])
        else:
            raise ValueError('Unverified grouping node')
    return result


def validate_frame(original, inventory, selection, launch_root, index):
    require(original['ok'], 'Original export failed')
    mapping = match_actions(original, inventory)
    calls = original['calls']
    require(len(calls) == original['api_call_count'] and len({x['id'] for x in calls}) == len(calls), 'Invalid call inventory')
    events = {x['id'] for x in calls if x['is_event']}
    require(len(events) == original['event_call_count'], 'Invalid event count')
    require(all(original['bindings'][str(e)]['ok'] for e in events), 'Event binding lookup failed')
    require(set(original['groupings']) == {'Render Target','Command List','Shader Set','Pipeline State','Debug Region'}, 'Missing grouping exports')
    require(all(events <= group_ids(nodes) for nodes in original['groupings'].values()), 'Grouping loses events')
    selected_ids = [entry['event'] for entry in selection['events']]
    has_dispatch = any(call['name'] == 'Dispatch' for call in mapping.values())
    require(len(set(selected_ids)) == len(selected_ids) == (3 if has_dispatch else 2), 'Missing selected events')
    selected = []
    for entry in selection['events']:
        event = entry['event']
        call = mapping[event]
        binding = original['bindings'][str(call['id'])]
        program = original['programs'][str(binding['execution']['program']['id'])]
        folder = launch_root/f'pipeline-{index}-{event}'
        report, pipeline = read(folder/'report.json'), read(folder/'replay-pipeline.json')
        require(report['completed'] and report['reference_pixels_used'] is False and int(report['event']) == event,
                'Invalid native pipeline evidence')
        fields = {f['field']:f for f in pipeline['fields']}
        stages = ('compute',) if call['name'] == 'Dispatch' else ('vertex','pixel')
        require(len(stages) == len(entry['shaders']), 'Unexpected shader selection')
        shaders = []
        for stage,sid in zip(stages,entry['shaders']):
            blob = launch_root/f'shader-{index}-{sid}/shader.dxbc'
            expected = disassemble(blob.read_bytes())
            require(expected and clean_assembly(program[stage]['dxbc']) == expected, f'Shader differs: {event}/{stage}')
            native_stage = {'vertex':'vs','pixel':'ps','compute':'cs'}[stage]+'.shader'
            require(fields[native_stage]['known'] and fields[native_stage]['value'] == sid, 'Native shader binding differs')
            shaders.append(dict(stage=stage, captured_id=sid, original_id=program[stage]['id'], sha256=digest(blob),
                                disassembly_sha256=hashlib.sha256(expected.encode()).hexdigest()))
        resources = binding['inputs'] + binding['outputs']
        require(resources, 'Selected event has no resources')
        for resource in resources:
            views = original['resources'][str(resource['resource_id'])]
            require(any(v['description'] == resource and call['id'] in v['usages'] and 'usage_error' not in v for v in views),
                    'Resource description or event usage missing')
        states = binding['execution']['states']
        if stages != ('compute',):
            require(graphics_state(fields) == states, 'Native and original graphics states differ: '+str(event))
        else:
            require(states == {}, 'Unexpected compute fixed-function states')
        selected.append(dict(captured_event=event, original_event=call['id'], name=call['name'], shaders=shaders,
                             resources=len(resources), graphics_state_groups=len(states)))
    return dict(events=len(events), ordered_actions=len(mapping), programs=len(original['programs']),
                resources=len(original['resources']), groupings=len(original['groupings']), selected=selected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    require(not args.out.exists(), 'Output must be new')
    manifest = read(args.manifest)
    result = dict(passed=False, formal_acceptance_established=False, frames=[], error=None,
                  manifest_sha256=digest(args.manifest), tool_sha256=digest(Path(__file__)))
    try:
        require(len(manifest['runs']) == 6, 'Expected six exports')
        require({(r['launch'],r['index']) for r in manifest['runs']} == {(l,i) for l in (1,2) for i in range(3)}, 'Duplicate or missing frame')
        for row in manifest['runs']:
            launch, index = row['launch'], row['index']
            require(row['exit_code'] == 0 and row['ok'] and digest(Path(row['export'])) == row['export_sha256'], 'Export provenance mismatch')
            root = args.evidence/f'launch-{launch}'
            inventory = read(root/f'inventory-{index}/report.json')
            require(Path(inventory['capture']).resolve() == Path(row['frame']).resolve() and digest(Path(row['frame'])) == row['frame_sha256'], 'Frame identity differs')
            selection = next(x for x in read(root/'inspection-selection.json') if x['index'] == index)
            details = validate_frame(read(Path(row['export'])), inventory, selection, root, index)
            result['frames'].append(dict(launch=launch,index=index,**details))
        result['passed'] = True
    except (OSError, ValueError, KeyError, TypeError, StopIteration) as error:
        result['error'] = f'{type(error).__name__}: {error}'
    with args.out.open('x',encoding='utf-8') as stream:
        json.dump(result,stream,indent=2)
        stream.write('\n')
    print(json.dumps(dict(passed=result['passed'], frames=len(result['frames']), error=result['error'])))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
