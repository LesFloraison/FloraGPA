"""Compare C++ initial membership with original file index, cache and ERG graph."""
import argparse
from pathlib import Path
import sys
from validate_compatibility_gate import digest,load,require
from validate_corpus import original_rgba,write
from validate_original_reference_evidence import check_references
from validate_original_init_traces import check_trace


def check_cache(trace,commands,entries):
    result=check_references(trace,commands)
    require(result['recovered']==result['observed'] and not result['unrecovered'],'Unresolved observed collector')
    first,last=trace['cache_before_first_collector'],trace['cache_before_first_initialize']
    require(first==last,'Cache changed during dependency collection')
    cpp=commands['original_initialization_cache']
    require(cpp['status']=='recovered' and cpp['scope']=='unmodified_initial_file_registration' and
            cpp['version']==0 and not cpp['resource_objects_validated'],'Overstated or incomplete cache scope')
    for key in ['categories','descriptors']: require(cpp[key]==first[key],'C++ cache differs: '+key)
    index=trace['original_file_index']
    require(len(index)==len(entries) and len({r['id'] for r in index})==len(entries),'Incomplete original index')
    expected=[]
    for ident,e in sorted(entries.items()):
        require(0<=ident<=0xffffffff and e['flags']==0 and e['category'] in [3,5,7,9], 'Unverified file index profile')
        expected.append(dict(id=ident,kind=e['category']//2,type=e['type']))
    require([{k:r[k] for k in ['id','kind','type']} for r in index]==expected,'Original index differs from capture')
    require([{k:r[k] for k in ['id','kind']} for r in index]==first['categories'],'Cache categories differ from original index')
    graph=trace['graph_before_first_erg_initialize']
    for kind,ids in first['descriptors'].items():
        require(ids==sorted(n['id'] for n in graph if n['kind']==kind),'Cache/graph descriptors differ')
    ergs=set(first['descriptors']['erg'])
    missing_ergs=sum(r['kind']==3 and r['id'] not in ergs for r in index)
    return dict(**result,index_entries=len(index),category_only_ergs=missing_ergs,
                descriptor_entries=sum(len(v) for v in first['descriptors'].values()))


def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--case',nargs=4,action='append',required=True,metavar=('TRACE','CONTROL','COMMANDS','CAPTURE'))
    parser.add_argument('--reference-python',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=False)
    sys.path.insert(0,str(args.reference_python.resolve(strict=True)))
    from frame import Frame
    report=dict(schema='FloraGPA original initial cache evidence 1',completed=False,passed=False,cases=[])
    for observed,control,commands,capture in args.case:
        observed,control,commands,capture=map(Path,[observed,control,commands,capture])
        trace,baseline=load(observed/'trace.json'),load(control/'trace.json')
        check_trace(baseline)
        require(not baseline['observers_enabled'],'Instrumented control')
        require(digest(capture)==trace['capture_sha256']==baseline['capture_sha256'],'Different capture')
        require(trace['player_sha256']==baseline['player_sha256'],'Different player')
        image=original_rgba(observed)
        require(image and image==original_rgba(control),'Different original pixels')
        with Frame(capture) as frame:
            result=check_cache(trace,load(commands),{ident:vars(e) for ident,e in frame.entries.items()})
        result.update(capture_sha256=trace['capture_sha256'],image_equal=True,
                      evidence={str(p):digest(p) for p in [observed/'trace.json',control/'trace.json',commands]})
        report['cases'].append(result);write(args.out/'validation.json',report)
    report.update(completed=True,passed=True,sources={p:digest(Path(__file__).parent/p) for p in
                  ['validate_initial_cache_evidence.py','validate_original_reference_evidence.py','validate_original_init_traces.py']})
    write(args.out/'validation.json',report)
    print('Cache entries:',sum(c['index_entries'] for c in report['cases']),
          'exact collectors:',sum(c['recovered'] for c in report['cases']))


if __name__=='__main__': main()
