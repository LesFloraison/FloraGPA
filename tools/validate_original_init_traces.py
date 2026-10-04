"""Serial observer/no-observer original-playback controls on unchanged captures."""
import argparse
from pathlib import Path
import os
import sys
from validate_compatibility_gate import digest, load, require
from validate_corpus import original_rgba, run, write


def check_trace(trace):
    require(trace['completed'] and trace['patches_restored'] and not trace['observer_errors'],
            'Incomplete trace or un-restored observer')
    replay = trace['replay']
    require(replay['closed'] and replay['open_status'] == replay['playback_status'] == 0 and
            not replay['callbacks'], 'Original replay did not complete')
    observations = trace['observations']
    if not trace['observers_enabled']:
        require(not observations and trace['patched_slots'] == 0, 'Control installed observers')
        return 0
    created = [o['event'] for o in observations if o['kind'] == 'create']
    entered = [o['event'] for o in observations if o['kind'] == 'initialize_enter']
    returned = [o['event'] for o in observations if o['kind'] == 'initialize_return']
    graph = trace['graph_before_first_erg_initialize']
    nodes = {n['id']: n for n in graph}
    ergs = sorted(n['id'] for n in graph if n['kind'] == 'erg')
    require(len(nodes) == len(graph) and ergs, 'Empty/duplicate graph')
    # This is an explicit observed scope, not a universal scheduling assumption.
    for ident in ergs:
        require(nodes[ident]['version'] == 0 and nodes[ident]['status'] == 1, 'Non-initial ERG node')
        require(all(d in nodes and nodes[d]['kind'] != 'erg' and nodes[d]['status'] == 2
                    for d in nodes[ident]['dependencies']), 'Different dependency scope; inspect trace')
    require(created == entered == returned == ergs, 'Actual factory/initialization order differs')
    require([o['kind'] for o in observations] == ['create'] * len(ergs) +
            ['initialize_enter', 'initialize_return'] * len(ergs), 'Unexpected nesting/lifecycle')
    require(len({o['thread'] for o in observations}) == 1, 'Unexpected concurrent initialization')
    require(len(trace['initializer_stacks']) == min(4,len(ergs)) and
            all(s['player_return_rvas'][:4] == ['0x9a703','0x9a815','0x9ad86','0x92a7c']
                for s in trace['initializer_stacks']), 'Different native scheduler call chain')
    return len(ergs)


def main():
    parser = argparse.ArgumentParser(__doc__)
    for name in ['merges', 'legacy', 'reference-tools', 'out']:
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).resolve().parents[1]
    cases = [(f'merge_{i}', args.merges / str(i) / 'capture.gpa_frame') for i in [0,16,31,32,48,63]]
    cases += [('gf2',args.legacy/'GF2_Exilium_2026_03_03__00_19_35.gpa_frame'),
              ('bf1',args.legacy/'bf1_2026_01_21__16_53_05.gpa_frame')]
    evidence = dict(schema='FloraGPA ERG initialization trace controls 1', completed=False, passed=False,
                    sources={p:digest(repo/p) for p in ['tools/trace_original_erg_initialization.py',
                                                       'tools/validate_original_init_traces.py']}, cases=[])
    write(args.out/'validation.json',evidence)
    env = os.environ.copy()
    env.pop('GPA_LOCAL_INJECT',None)
    for name, capture in cases:
        case = dict(id=name,capture=str(capture.resolve()),capture_sha256=digest(capture),runs=[])
        evidence['cases'].append(case)
        images = []
        for observe in [False,True]:
            tag = name + ('-observed' if observe else '-control')
            folder = args.out/tag
            command = [sys.executable,repo/'tools/trace_original_erg_initialization.py',capture.resolve(),
                       '--reference-tools',args.reference_tools.resolve(),'--out',folder.resolve()]
            if not observe:
                command.append('--no-observers')
            process = run(command,args.out/(tag+'.log'),180,env)
            case['runs'].append(process)
            write(args.out/'validation.json',evidence)
            require(process['exit_code'] == 0 and not process['timed_out'], 'Trace subprocess failed: '+tag)
            trace = load(folder/'trace.json')
            require(trace['capture_sha256'] == case['capture_sha256'], 'Wrong observed capture')
            process.update(trace_sha256=digest(folder/'trace.json'),ergs_checked=check_trace(trace))
            image = original_rgba(folder)
            require(image, 'Missing original raw image')
            images.append(image)
        require(images[0] == images[1], 'Observers changed original replay pixels')
        case.update(observed_image_equal=True,rgba_bytes=len(images[0]))
        write(args.out/'validation.json',evidence)
        print(name,case['runs'][1]['ergs_checked'],'original pixels unchanged',flush=True)
    evidence.update(completed=True,passed=True,original_runs=len(cases)*2,
                    ergs_checked=sum(c['runs'][1]['ergs_checked'] for c in evidence['cases']))
    write(args.out/'validation.json',evidence)


if __name__ == '__main__':
    main()
