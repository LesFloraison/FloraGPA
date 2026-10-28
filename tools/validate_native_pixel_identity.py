"""Independently check typed predicates and conditional replay identity joins."""
import argparse
import copy
import hashlib
import json
import math
import struct
from pathlib import Path
from validate_native_pixel_completion import require, shader_bytes
from validate_native_pixel_primitive_mask import shader

MARKER = (0x3f800001, 0x40000001, 0x40400001)


def predicate(row):
    require(row['schema'] == 1 and row['kind'] == 'native_pixel_predicate_decode', 'Predicate decode schema')
    fmt, raw = row['typed_format'], bytes(row['bytes'])
    ordinary = {2:('4f',3),3:('4I',3),4:('4i',3),10:('4e',3),11:('4H',3),12:('4H',3),
                13:('4h',3),14:('4h',3),16:('2f',2),17:('2I',2),
                28:('4B',3),29:('4B',3),30:('4B',3),31:('4b',3),32:('4b',3),
                41:('f',1),42:('I',1),43:('i',1),49:('2B',2),50:('2B',2),
                61:('B',1),62:('B',1),63:('b',1),64:('b',1),
                87:('4B',3),88:('4B',3),91:('4B',3)}
    if fmt in ordinary:
        encoding, count = ordinary[fmt]
        rgb = struct.unpack('<' + encoding, raw)[:count]
    elif fmt in (24,25):
        word, = struct.unpack('<I',raw)
        rgb = [(word >> shift) & 1023 for shift in (0,10,20)]
    elif fmt == 26:
        word, = struct.unpack('<I',raw)
        rgb = []
        for shift, mantissa in ((0,6),(11,6),(22,5)):
            value = (word >> shift) & ((1 << (mantissa + 5))-1)
            rgb.append(float('nan') if value >> mantissa == 31 else value)
    elif fmt in (1,65):
        require(row['rgb_predicate'] is None, 'Unstored or typeless color remains unknown')
        return None
    else:
        raise ValueError('Unvalidated predicate format: ' + str(fmt))
    expected = False if all(v == 0 for v in rgb) else True if all(math.isfinite(v) and v > 0 for v in rgb) else None
    require(row['rgb_predicate'] is expected, 'Typed raw RGB predicate')
    color = row['decoded']['color']
    require(row['decoded']['status'] == 'decoded' and color is not None, 'Typed color result')
    stored = color['stored_channels']
    require(stored[:3] == [i < len(rgb) for i in range(3)], 'Only stored RGB participates')
    values = [color['rgba'][i] for i in range(3) if stored[i]]
    decoded = False if all(v == 0 for v in values) else True if all(type(v) in (int,float) and math.isfinite(v) and v > 0 for v in values) else None
    require(decoded is expected, 'Decoded color agrees with raw predicate')
    return expected


def validate(r):
    require(r['schema'] == 1 and r['kind'] == 'native_pixel_identity_observation' and
            r['backend'] == 'native-pixel-identity/1' and
            r['scope'] == 'repeated_original_ps_private_color_predicates', 'Identity observation schema')
    require(r['original_fragment_eligibility'] is None and r['primitive_ordinal'] is None and
            r['instance'] is None and r['original_gpu_schedule'] is False, 'Unproved invocation fields')
    passes = r['passes']
    require(len(passes) == r['pass_count'] and 1 <= len(passes) <= 36, 'Bounded pass count')
    require(0 < r['retained_payload_bytes'] <= r['payload_limit'] and 0 < r['timeout_ms'] <= 60000 and
            0 <= r['elapsed_ms'] <= r['timeout_ms'], 'Payload and time bounds')
    candidate, values = 0, []
    for index, p in enumerate(passes):
        obs = p['observation']
        s = obs['completion_shader']
        require(obs['frame_sha256'] == r['frame_sha256'] and obs['event'] == r['event'] and
                obs['adapter'] == r['adapter'] and obs['shaders'] == r['shaders'] and
                obs['completion_target'] == r['completion_target'] and s['target'] == r['target'], 'Same pass context')
        require(s['marker_bits'] == list(MARKER) and s['source_sha256'] == r['shaders'][4]['sha256'] and
                s['alpha_preserved'] is True and s['extra_uav_slots'] == 0 and
                s['original_fragment_eligibility'] is None and obs['original_fragment_eligibility'] is None,
                'Original PS and marker scope')
        require(obs['diagnostic_shader_replacements'] is True and obs['original_gpu_schedule'] is False and
                obs['completion_target']['cleared_to_zero'] is True and
                obs['completion_target']['blend_and_logic_disabled'] is True and
                obs['completion_target']['write_mask'] == 15, 'Diagnostic output setup')
        require(p['before']['typed_format'] == p['after']['typed_format'] == r['completion_target']['format'], 'Reader format')
        before, after = predicate(p['before']), predicate(p['after'])
        require(before is not True, 'Cleared RGB')
        value = after if before is False else None
        values.append(value)
        role = 'reference' if index == 0 else 'bit' if index <= 32 else 'equal' if index == 33 else 'different' if index == 34 else 'reference_repeat'
        require(p['role'] == role, 'Exact ordered pass role')
        if role in ('reference', 'reference_repeat'):
            require(obs['kind'] == 'native_pixel_completion_observation' and
                    s['kind'] == 'native_pixel_completion_shader' and s['extra_input_registers'] == 0 and
                    s['extra_temps'] == 0, 'Uniform completion reference')
        else:
            require(obs['kind'] == 'native_pixel_primitive_mask_observation' and
                    s['kind'] == 'native_pixel_primitive_mask_shader' and s['extra_temps'] == 1 and
                    obs['primitive_ordinal'] is None and obs['instance'] is None, 'System predicate scope')
            mask = 1 << (index-1) if role == 'bit' else 0xffffffff
            value_mask = mask if role == 'bit' else candidate if role == 'equal' else candidate ^ 1
            require(s['primitive_mask'] == mask and s['primitive_value'] == value_mask, 'Predicate argument')
            if role == 'bit' and value is True:
                candidate |= mask
    if values[0] is None or None in values[1:33]:
        expected = 'uninterpretable'
        require(r['candidate_system_id'] is None, 'No candidate from unknown bits')
    elif values[0] is False:
        expected = 'no_color_completion'
        require(len(passes) == 1 and r['candidate_system_id'] is None, 'No completion stops identity inference')
    else:
        require(len(passes) == 36 and r['candidate_system_id'] == candidate, 'All bits and independent full predicates')
        expected = 'verified_conditional_replay' if values[33] is True and values[34] is False and values[35] is True else 'inconsistent'
    require(r['status'] == expected and r['system_primitive_id'] == (candidate if expected == 'verified_conditional_replay' else None),
            'Verified identity publication')
    return r


def validate_history_identity(history):
    identity, association = history['primitive_identity_observation'], history['primitive_identity_association']
    require(identity['requested'] is True and association['scope'] == 'system_id_equality_only' and
            association['original_fragment_eligibility'] is None, 'History identity association scope')
    if identity['status'] == 'not_applicable':
        require(identity['reason'] in ('selected_depth_target','no_pixel_shader') and association['status'] == 'not_observed', 'Inapplicable identity')
        return 0
    validate(identity)
    require(identity['frame_sha256'] == history['frame_sha256'] and identity['event'] == history['event'] and
            identity['adapter'] == history['adapter'] and identity['shaders'] == history['original_draw_observation']['shaders'] and
            identity['target'] == history['selection']['subresource']['slot'], 'History context join')
    matching, unknown = [], []
    if identity['status'] == 'verified_conditional_replay':
        for row in history['records']:
            identifier = row['original_primitive_id']
            expected = None if identifier is None else identifier == identity['system_primitive_id']
            require(row['system_id_matches_verified_predicate'] is expected and row['original_fragment_eligibility'] is None,
                    'Per-candidate system identity equality')
            if identifier is None: unknown.append(row['primitive_ordinal'])
            elif expected: matching.append(row['primitive_ordinal'])
        status = 'no_matching_candidate' if not matching else 'one_matching_candidate' if len(matching) == 1 else 'ambiguous_candidates'
    else:
        status = 'not_observed'
    require(association['matching_primitive_ordinals'] == matching and association['unknown_identity_ordinals'] == unknown and
            association['status'] == status, 'All matching and unknown candidates retained')
    return 2 * identity['pass_count']


def main():
    from validate_native_pixel_draw_history import validate as validate_storage_history
    from validate_native_pixel_history_merger import validate as validate_merger_history
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--real', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    paths = sorted(args.directory.glob('*.json'))
    require(len(paths) == 30, 'Owned predicate matrix including repeated IDs')
    checks, controls, files = [], 0, []
    for path in paths:
        r = json.loads(path.read_text(encoding='utf-8'))
        source = Path(str(path)+'.original.dxbc').read_bytes()
        reference = Path(str(path)+'.reference.dxbc').read_bytes()
        for h in r['histories']:
            validate_history_identity(h)
            validate_storage_history(h,r['vendor_request'])
            require(h['primitive_identity_observation']['system_primitive_id'] == r['expected_system_primitive_id'], 'Owned authored identity')
            for index, row in enumerate(h['primitive_identity_observation']['passes']):
                s = row['observation']['completion_shader']
                if index in (0,35): shader_bytes(s,source,reference,MARKER)
                else: shader(s,source,Path(str(path)+f'.{index-1}.dxbc').read_bytes())
            checks.append(h)
        if r['identity_recovery'] is not None:
            recovery=r['identity_recovery']
            require(recovery['cancelled_after_reads']==5 and recovery['wrong_format_rejected'] is True and recovery['storage_restored'] is True,'Recovery boundaries')
            validate(recovery['retry']); validate(recovery['masked'])
            validate(recovery['fault_injection'])
            require(recovery['injected_equal_readback_zero'] is True and recovery['fault_injection']['status']=='inconsistent' and
                    recovery['fault_injection']['system_primitive_id'] is None, 'Injected full-width verification failure')
            require(recovery['retry']['system_primitive_id']==r['expected_system_primitive_id'] and recovery['masked']['status']=='no_color_completion','Recovery result')
        files.append({'path':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
    require(len(checks)==39,'All owned samples joined including repeated IDs')
    real=sorted(args.real.glob('*/report.json')) if args.real else []
    if args.real: require(len(real)==9,'Real capture matrix')
    for path in real:
        r=json.loads(path.read_text(encoding='utf-8')); validate_history_identity(r)
        validate_merger_history(r,0 if r['adapter']['warp'] else r['adapter']['vendor'])
        require(r['probe']['status']=='ok' and bytes(r['after']['bytes'])==(path.parent/'reference.bin').read_bytes(), 'Independent real replay')
        checks.append(r); files.append({'path':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
    for h in checks:
        for change in ('system_id','role','predicate','eligibility','association'):
            bad=copy.deepcopy(h); identity=bad['primitive_identity_observation']
            if change=='system_id': identity['system_primitive_id']=(identity['system_primitive_id'] or 0)^1
            elif change=='role': identity['passes'][0]['role']='invalid'
            elif change=='predicate': identity['passes'][0]['after']['rgb_predicate']=not identity['passes'][0]['after']['rgb_predicate']
            elif change=='eligibility': identity['original_fragment_eligibility']=True
            else: bad['primitive_identity_association']['matching_primitive_ordinals']=[999999999]
            try: validate_history_identity(bad)
            except (ValueError,TypeError,IndexError): controls+=1; continue
            raise ValueError('Accepted identity corruption: '+change)
    decode_path=Path(str(args.directory)+'.decode.json')
    decoded=json.loads(decode_path.read_text(encoding='utf-8'))
    for row in decoded: predicate(row)
    result={'schema':1,'status':'ok','owned_histories':39,'real_histories':len(real),
            'predicate_passes':sum(h['primitive_identity_observation']['pass_count'] for h in checks),
            'decoder_cases':len(decoded),'negative_controls':controls,'complete_pixel_history':False,'files':files}
    with args.out.open('x',encoding='utf-8') as output: json.dump(result,output,indent=2);output.write('\n')
    print(json.dumps({k:v for k,v in result.items() if k!='files'}))


if __name__=='__main__': main()
