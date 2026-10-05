"""Verify native version primitives, real cache clones and bounded reload traces."""
import argparse
from pathlib import Path
import sys
from probe_original_reference_collectors import PLAYER
from validate_compatibility_gate import digest, load, require
from validate_corpus import original_rgba, write
from validate_original_init_traces import check_trace


def unique(rows):
    result={n['id']:n for n in rows}
    require(len(result)==len(rows),'Duplicate graph identity')
    return result


def check_primitives(report):
    require(report['completed'] and report['passed'] and report['player_sha256']==PLAYER and
            not report['gpu_execution'] and not report['original_capture_acceptance'],'Overstated primitive scope')
    cases={c['kind']:c for c in report['cases']}
    require(len(cases)==len(report['cases'])==4 and set(cases)=={'state','resource','erg','data'},'Missing primitive kind')
    def state(deps=(),children=(),token=0,status=0):
        return dict(status=status,dependencies=list(deps),dependents=list(children),object_token=token)
    total=0
    for kind,case in cases.items():
        a=[11,22] if kind in ['erg','data'] else [11];b=sorted(a+[33]);empty=[0] if kind=='erg' else []
        original=state(a,[200],1,1);modified=state(b,[200,201],2,1)
        after_empty=state(sorted(set(b+empty)),[200,201],2,1)
        expected=[('fresh',{'0':state()}),('collected',{'0':original}),
            ('sparse_growth',{'0':original,'1':state(),'7':state()}),('copied',{'0':original,'7':original}),
            ('recollected',{'0':original,'7':modified}),('empty_recollection',{'0':original,'7':after_empty}),
            ('fresh_empty',{'0':original,'1':state(empty,[],3,1),'7':after_empty}),
            ('copy_replaces_sets',{'0':original,'1':state(empty,[],3,1),'7':original})]
        require(case['passed'] and len(case['steps'])==len(expected),'Incomplete primitive sequence')
        for row,(label,nodes) in zip(case['steps'],expected):
            require(row['label']==label and row['nodes']==nodes and row['expected']==nodes,'Different primitive transition')
            total+=len(nodes)
    return dict(kinds=4,transitions=32,node_snapshots=total)


def check_versions(report,initial_graph):
    require(report['completed'] and report['passed'] and report['closed'] and report['open_status']==0 and
            report['observers_restored'] and not report['observer_errors'] and not report['callbacks'],
            'Incomplete native version run')
    require(report['player_sha256']==PLAYER and not report['cloned_version_workload_playback'] and
            report['cloned_version_resource_initialization'],'Overstated or missing reload scope')
    require(report['default_pixels_equal'] and report['default_graph_unchanged_after_playback'],
            'Default version changed')
    playbacks=report['default_playbacks']
    require([{k:r[k] for k in ['label','status','error']} for r in playbacks]==
            [dict(label=label,status=0,error=0) for label in ['before','after']],
            'Missing default playback control')
    require(report['default_execution_counts_equal'] and playbacks[0]['audit']==playbacks[1]['audit'] and
            playbacks[0]['audit']['event_dispatch_count']>0 and
            not playbacks[0]['audit']['negative_control_suppress_draws'],'Default execution counts changed')
    require(report['initial_versions']==[0],'Unexpected original version registry')
    initial=unique(report['initial_graph']);expected=unique(initial_graph['nodes'])
    require(report['nodes']==len(initial) and set(initial)==set(expected),'Different initial node inventory')
    audit=playbacks[0]['audit']
    require(audit['event_dispatch_count']==sum(n['kind']=='erg' for n in initial.values()) and
            sum(audit['event_function_rvas'].values())==audit['event_dispatch_count'],'Incomplete default ERG execution count')
    require(initial_graph['dependency_graph_complete'] and not initial_graph['initialization_schedule_available'] and
            not initial_graph['execution_supported'],'Unaccepted initial metadata scope')
    reverse={i:[] for i in initial}
    for ident,node in initial.items():
        require(node['dependencies']==sorted(set(node['dependencies'])),'Duplicate or unsorted native dependency')
        for dependency in node['dependencies']:
            require(dependency in reverse,'Missing native dependency')
            reverse[dependency].append(ident)
    for ident,node in initial.items():
        require(node['status']==2 and node['kind']==expected[ident]['kind'] and
                node['dependencies']==expected[ident]['dependency_set'],'Different initial dependencies/readiness')
        require(node['object_identity']!='0x0' and node['control_identity']!='0x0','Empty initialized object')
        require(node['dependents']==sorted(reverse[ident]),
                'Different initial reverse dependency set')
    holes=unique(report['unregistered_hole_graph'])
    require(set(holes)==set(initial),'Incomplete sparse version')
    for ident,node in holes.items():
        require(node==dict(id=ident,kind=initial[ident]['kind'],status=0,dependencies=[],dependents=[],
                           object_identity='0x0',control_identity='0x0'),'Allocated hole is not blank')
    clones=report['clones'];require(len(clones)==3,'Missing native clone')
    require([(r['source'],r['target']) for r in clones]==[(0,7),(7,9),(7,11)],'Different clone chain')
    for index,row in enumerate(clones):
        require(row['equal'] and row['default_unchanged'] and
                row['registered_versions']==[0,7,9,11][:index+2], 'Invalid clone registry/default isolation')
        require(unique(row['source_graph'])==unique(row['target_graph']),'Clone loses objects/graph metadata')
        if index<2:require(unique(row['source_graph'])==initial,'Clone differs from source before editing')
    previous=initial;calls=0;kinds=[]
    reloads=report['saved_payload_reloads'];require(len(reloads)==4,'Missing one of four reload categories')
    rvas={'state':'0x9a710','resource':'0x9a6d0','erg':'0x9a6f0','data':'0x9a6d0'}
    category={'state':3,'resource':5,'erg':7,'data':9}
    for row in reloads:
        before,after=unique(row['before']),unique(row['after']);ident=row['id']
        require(before==previous and set(after)==set(before) and ident in before,'Broken reload continuity')
        target=before[ident];kinds.append(target['kind'])
        require(row['category']==category[target['kind']] and row['type']==expected[ident]['type'],
                'Reload category/type differs')
        require(row['default_unchanged'] and row['other_clone_unchanged'],'Reload changes another version')
        for key,node in after.items():
            if key!=ident:require(node==before[key],'Reload changes another node wrapper/metadata')
            else:
                require(all(node[k]==target[k] for k in ['id','kind','status','dependencies','dependents']),
                        'Identical payload changes logical node metadata')
                require(all(node[k]!='0x0' and node[k]!=target[k] for k in ['object_identity','control_identity']),
                        'Nonzero version did not replace target wrapper')
        expected_calls=[ident]+target['dependents']
        require([c['id'] for c in row['initializers']]==expected_calls,'Different propagation order/depth')
        for index,call in enumerate(row['initializers']):
            kind=before[call['id']]['kind']
            require(call==dict(id=expected_calls[index],kind=kind,version=7,initializer_rva=rvas[kind],
                              before_status=1 if index==0 else 2,after_status=2),'Different initializer state/version')
        calls+=len(expected_calls);previous=after
    require(sorted(kinds)==['data','erg','resource','state'],'Reload categories are incomplete')
    require(unique(clones[2]['source_graph'])==previous,'Final clone omits changed wrappers')
    return dict(nodes=len(initial),clones=3,copied_nodes=3*len(initial),reloads=4,initializer_calls=calls)


def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--primitives',type=Path,required=True)
    parser.add_argument('--case',nargs=4,action='append',required=True,metavar=('RUN','CONTROL','GRAPH','CAPTURE'))
    parser.add_argument('--reference-python',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=False)
    sys.path.insert(0,str(args.reference_python.resolve(strict=True)))
    from frame import Frame
    report=dict(schema='FloraGPA original version evidence 1',completed=False,passed=False,
                primitives=check_primitives(load(args.primitives)),cases=[])
    for paths in args.case:
        folder,control,graph,capture=map(Path,paths);native=load(folder/'versions.json');baseline=load(control/'trace.json')
        check_trace(baseline);require(not baseline['observers_enabled'],'Instrumented pixel control')
        require(native['capture_sha256']==baseline['capture_sha256']==digest(capture),'Different original capture')
        result=check_versions(native,load(graph))
        with Frame(capture) as frame:
            for row in native['saved_payload_reloads']:
                import hashlib
                require(row['payload_sha256']==hashlib.sha256(frame.payload(row['id'])).hexdigest(),
                        'Reload payload differs from unchanged capture')
        image=original_rgba(folder/'before')
        require(image and image==original_rgba(folder/'after')==original_rgba(control),'Default raw image differs')
        result.update(capture_sha256=native['capture_sha256'],default_pixels_equal=True,
                      evidence={str(p):digest(p) for p in [folder/'versions.json',control/'trace.json',graph]})
        report['cases'].append(result);write(args.out/'validation.json',report)
    report.update(completed=True,passed=True,source_sha256=digest(Path(__file__)),primitives_sha256=digest(args.primitives))
    write(args.out/'validation.json',report)
    print('Verified native clones:',sum(r['clones'] for r in report['cases']),
          'reload initializers:',sum(r['initializer_calls'] for r in report['cases']))


if __name__=='__main__':main()
