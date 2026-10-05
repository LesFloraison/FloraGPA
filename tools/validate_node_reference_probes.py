"""Compare native private-ABI marker records with the independent C++ decoder.

The generated capture is a CPU metadata fixture, never an original capture or a
GPU replay acceptance case. Missing marker dependencies are deliberately retained.
"""
import argparse
import os
from pathlib import Path
import struct
from validate_compatibility_gate import digest, load, require
from validate_corpus import run, write
from probe_original_reference_collectors import PLAYER


def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--state', type=Path, help='Optional additional state probe report')
    for key in ['nodes','exe','qt-bin','out']:parser.add_argument('--'+key,type=Path,required=True)
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=False)
    records=[]
    inputs = [p for p in [args.state, args.nodes] if p is not None]
    for path in inputs:
        report=load(path)
        require(report['completed'] and report['passed'] and not report['gpu_execution'] and
                report['player_sha256']==PLAYER,'Incomplete or different native probe')
        for row in report['cases']:
            records.append(dict(row,category=row.get('category',3),type=row.get('type',3),
                                collector_rva=row.get('collector_rva','0x63d80')))
    require(bool(records), 'No native probe records')
    data=bytearray(0x128);table=[]
    for ident,row in enumerate(records,1):
        raw=bytes.fromhex(row['raw_hex']);table.append((ident,len(data),len(raw),0,row['category'],row['type']))
        data.extend(raw)
    struct.pack_into('<IIII',data,0,0x41504749,0x128,3,len(table));data[0x44:0x48]=b'DX11'
    struct.pack_into('<Q',data,0xf4,len(data))
    for entry in table:data.extend(struct.pack('<QQIBBH',*entry))
    capture=args.out/'markers.gpa_frame';capture.write_bytes(data)
    env=os.environ.copy();env['PATH']=str(args.qt_bin.resolve())+os.pathsep+env.get('PATH','')
    env.pop('GPA_LOCAL_INJECT',None)
    process=run([args.exe.resolve(),'initialization-graph',capture.resolve(),'--out',(args.out/'cpp').resolve()],
                args.out/'cpp.log',60,env)
    require(process['exit_code']==0 and not process['timed_out'],'C++ graph export failed')
    graph=load(args.out/'cpp/initialization-graph.json');nodes={n['id']:n for n in graph['nodes']}
    require(len(nodes)==len(records),'Missing marker node')
    for ident,row in enumerate(records,1):
        node=nodes[ident]
        require(node['status']=='recovered' and node['collector_rva']==row['collector_rva'],'Different marker decoder')
        require(node['collector_sequence']==row['references'],'Different marker order/duplicates')
        require(node['dependency_set']==sorted(set(row['references'])),'Different marker dependencies')
    require(not graph['dependency_graph_complete'] and graph['issues'] and
            not graph['execution_supported'] and not graph['initialization_schedule_available'],
            'Marker identities must not claim executable graph')
    write(args.out/'validation.json',dict(schema='FloraGPA node marker comparison 1',completed=True,passed=True,
        records=len(records),gpu_execution=False,capture_is_original=False,
        evidence={str(p):digest(p) for p in inputs + [capture,args.out/'cpp/initialization-graph.json']},
        source_sha256=digest(Path(__file__)),exe_sha256=digest(args.exe)))
    print('Exact native/C++ marker collectors:',len(records))


if __name__=='__main__':main()
