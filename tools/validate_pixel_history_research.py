"""Validate independent draw observations and scoped original history conclusions."""
import argparse
import copy
import json
from pathlib import Path
import re
from probe_original_debugger_host import digest, PLAYER_SHA256

NAMES=('write','unchanged_write','scissor_reject','depth_reject','stencil_reject','discard',
       'culled','clipped','blend','color_mask_zero','sample_mask_zero','overlap')


def require(value,message):
    if not value: raise ValueError(message)


def load(path): return json.loads(path.read_text(encoding='utf-8-sig'))


def image_expected(case,x,y):
    rejected=case in (2,3,4,5,6,7,10)
    writes=(not rejected or case==2 and x<2 and y<2) and case not in (1,9)
    return [128,0,128,128] if case==8 else [255,0,0,255] if writes else [0,0,255,255]


def reference(root,vendor,captured=False):
    r=load(root/'observations.json')
    require(r['completed'] and r['scope']=='independent_pixel_history_draw_observations','Incomplete reference')
    require(r['requested_vendor']==vendor and r['adapter']['vendor']==(vendor or 5140),'Reference GPU differs')
    require(r['capture_requested']==captured and len(r['frames'])==8,'Reference run shape differs')
    if not captured: require((root/'debug.txt').read_bytes()==b'','Reference debug diagnostics present')
    exact=tolerated=0
    for frame in r['frames']:
        require(len(frame['cases'])==12 and 0<=frame['frame']<8,'Reference cases missing')
        for i,row in enumerate(frame['cases']):
            require((row['case'],row['name'])==(i,NAMES[i]) and row['image_verified'],'Reference case identity differs')
            samples=4 if i==2 else 0 if i in (3,4,5,6,7,10) else 128 if i==11 else 64
            require(row['samples']==row['expected_samples']==samples,'Independent occlusion result differs')
            pixels=(root/f"frame{frame['frame']}-{NAMES[i]}.rgba").read_bytes()
            require(len(pixels)==256 and row['selected_pixel']==list(pixels[144:148]),'Reference image shape differs')
            for y in range(8):
                for x in range(8):
                    expected=image_expected(i,x,y)
                    actual=pixels[(y*8+x)*4:(y*8+x+1)*4]
                    require(all(abs(a-b)<=(1 if i==8 else 0) for a,b in zip(actual,expected)),'Independent pixel differs')
                    if i==8: tolerated+=4
                    else: exact+=4
    require({f['frame'] for f in r['frames']}==set(range(8)),'Reference frame sequence incomplete')
    return r,exact,tolerated


def history(r,events,adapter,capture_sha,reference_dir):
    require(r['completed'] and r['closed'] and r['player_unchanged'] and r['capture_unchanged'],'Incomplete lifecycle')
    require(r['player_sha256']==PLAYER_SHA256 and r['capture_sha256']==capture_sha,'Original input identity differs')
    require(r['scope']=='original_event_pixel_history_raw_vectors' and r['event_filter']==events,'History selection differs')
    require(r['version']==0 and r['selection']['mip']==r['selection']['layer']==0,'Unexpected version/subresource')
    require(not r['errors'] and not r['callback_errors'],'Original observer failure')
    actual=r['replay_adapter']
    require(actual==r['replay_adapter_after'] and all(actual[k]==adapter[k] for k in ('vendor','device','luid')),
            'Original adapter differs from reference')
    require(len(r['playbacks'])==2 and all(p['status']==p['error']==0 for p in r['playbacks']),'Replay failed')
    require(len(r['requests'])==2 and not r['requests'][0]['observed'] and r['requests'][1]['observed'],
            'Missing control or observed request')
    selected=[i for i in range(12) if i not in (6,7)]
    xy=r['selection']['x']; require(xy==r['selection']['y'] and xy in (1,4),'Unexpected pixel')
    for request in r['requests']:
        require(request['phase']=='returned' and request['status']==request['error']==0 and request['observer_restored'],
                'Original request or observer restoration failed')
        require(len(request['results'])==1,'Expected one original result')
        result=request['results'][0]
        require(result['events']==[events[i] for i in selected],'Original event vector differs')
        require(result['raw_status']==[2 if i==3 else 3 if i==4 else 5 if i==5 else 0 for i in selected],
                'Scoped original status mapping differs')
        require(result['diagnostic_words']==[2 if i==11 else 1 for i in selected], 'Diagnostic count differs')
        pixels=bytes.fromhex(result['pixel_bytes']); require(len(pixels)==4*len(selected),'Unexpected pixel payload')
        for n,i in enumerate(selected):
            raw=(reference_dir/f'frame4-{NAMES[i]}.rgba').read_bytes()
            require(pixels[4*n:4*n+4]==raw[(xy*8+xy)*4:(xy*8+xy+1)*4],'Original pixel differs from same-GPU reference')
    require(r['requests'][0]['results']==r['requests'][1]['results'],'Observer affected results')
    dispatch=r['requests'][1]['dispatch']; require(len(dispatch)==24,'Incomplete selected dispatch')
    for index,row in enumerate(dispatch):
        i,p=divmod(index,2)
        require((row['event'],row['mode'],row['pass_index'],row['flags'])==(events[i],1,p,0),'Dispatch identity differs')
        require(row['erg_vtable_rva']=='0x176038','Unexpected draw vtable')
        expected_event=0 if i in (6,7) else events[i]
        require(row['after']['event']==expected_event,'Manager event field is not the selected ERG identity')
        require(row['after']['diagnostic']==(0 if i in (6,7) else 2 if i==11 else 1),'Manager count differs')
        expected_status=(2 if i==3 else 3 if i==4 else 5 if i==5 else 0) if p==0 else 0
        require(row['after']['status']==expected_status,'Manager status chain differs')
    return len(selected)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory',type=Path)
    p.add_argument('--sdk-header',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    args=p.parse_args(); root=args.directory
    manifest=load(root/'manifest.json')
    require(manifest['completed'] and manifest['gpa_unchanged'] and manifest['capture_unchanged'],'Incomplete matrix')
    require(all(r['completed'] and r['exit_code']==0 for r in manifest['runs']) and len(manifest['runs'])==9,'Missing run')
    for name,value in manifest['producer_files'].items():
        require(digest(root/'producer'/name)==value,'Frozen producer changed')
    capture_sha=digest(root/'fixture.gpa_frame')
    require(capture_sha==manifest['capture']['sha256'],'Capture changed')
    refs={}; exact=tolerated=0
    for vendor in (0,4318,32902):
        r,e,t=reference(root/f'reference-{vendor}',vendor); refs[vendor]=r; exact+=e; tolerated+=t
    _,e,t=reference(root/'captured',4318,True); exact+=e; tolerated+=t
    commands=load(root/'commands/commands.json')['commands']
    events=[c['id'] for c in commands if c['name']=='Draw']
    require(events==manifest['events'] and len(events)==12,'Captured event mapping differs')
    counts=[f['value'] for c in commands if c['name']=='Draw' for f in c['fields'] if f['name']=='vertex_count']
    require(counts==[3]*11+[6],'Captured overlap workload differs')
    controls=[]; runs=[]
    for vendor in (4318,32902):
        for xy in (4,1):
            path=root/f'history-{vendor}-{xy}'/'history.json'; r=load(path)
            require(r['selection']['resource']==manifest['target'],'Wrong selected target')
            count=history(r,events,refs[vendor]['adapter'],capture_sha,root/f'reference-{vendor}')
            runs.append(dict(path=str(path),vendor=vendor,pixel=[xy,xy],events=count,
                             omitted_cases=['culled','clipped'],
                             eligibility_mismatches=['scissor_reject','sample_mask_zero'] if xy==4 else ['sample_mask_zero']))
            for name in ('pixel','status','count','event','adapter','scope','restoration','manager_event','dispatch','errors'):
                bad=copy.deepcopy(r)
                if name=='pixel': bad['requests'][0]['results'][0]['pixel_bytes']='00'+bad['requests'][0]['results'][0]['pixel_bytes'][2:]
                elif name=='status': bad['requests'][0]['results'][0]['raw_status'][3]=0
                elif name=='count': bad['requests'][0]['results'][0]['diagnostic_words'][-1]=1
                elif name=='event': bad['event_filter'][0]+=1
                elif name=='adapter': bad['replay_adapter']['luid']^=1
                elif name=='scope': bad['scope']='per_fragment_truth'
                elif name=='restoration': bad['requests'][1]['observer_restored']=False
                elif name=='manager_event': bad['requests'][1]['dispatch'][0]['after']['event']=1
                elif name=='dispatch': bad['requests'][1]['dispatch'][0]['pass_index']=1
                else: bad['callback_errors']=['injected']
                try: history(bad,events,refs[vendor]['adapter'],capture_sha,root/f'reference-{vendor}')
                except ValueError as error: controls.append(dict(vendor=vendor,pixel=xy,case=name,reason=str(error)))
                else: raise ValueError('Altered history evidence accepted: '+name)
    header=args.sdk_header.read_text(encoding='utf-8-sig')
    block=header.split('typedef struct ID3D11DeviceContextVtbl',1)[1].split('END_INTERFACE',1)[0]
    names=re.findall(r'\(\s*STDMETHODCALLTYPE\s*\*(\w+)\s*\)',block)
    expected={'PSSetShader':0x48,'Map':0x70,'Unmap':0x78,'Begin':0xd8,'End':0xe0,'GetData':0xe8,
              'OMSetRenderTargets':0x108,'OMSetBlendState':0x118,'OMSetDepthStencilState':0x120,
              'RSSetState':0x158,'RSSetScissorRects':0x168,'CopySubresourceRegion':0x170,
              'ClearRenderTargetView':0x190}
    slots={name:names.index(name)*8 for name in expected}
    require(slots==expected,'SDK context slots differ')
    artifacts=[dict(path=str(path),bytes=path.stat().st_size,sha256=digest(path)) for path in sorted(root.rglob('*')) if path.is_file()]
    result=dict(schema=1,scope='scoped_event_diagnostics_not_complete_fragment_history',complete_release_acceptance=False,
                reference_draws=384,exact_reference_components=exact,tolerated_unorm_blend_components=tolerated,
                original_requests=8,original_result_records=80,observed_dispatches=96,runs=runs,
                negative_controls=controls,sdk=dict(path=str(args.sdk_header),sha256=digest(args.sdk_header),slots=slots),artifacts=artifacts)
    with args.out.open('x',encoding='utf-8') as f: json.dump(result,f,indent=2); f.write('\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ('runs','negative_controls','artifacts','sdk')}))


if __name__=='__main__': main()
