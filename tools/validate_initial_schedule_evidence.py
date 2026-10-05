"""Compare full original initialization observations with the C++ schedule model.

Runs independent CPU export; original GPU observations must be collected serially
beforehand. This validates predicted initialization under successful native
callbacks, not a replacement executor or traditional list acceptance.
"""
import argparse
from collections import Counter
import os
from pathlib import Path
from probe_original_reference_collectors import PLAYER
from validate_compatibility_gate import digest, load, require
from validate_corpus import original_rgba, run, write
from validate_original_init_traces import check_trace


def check_schedule(trace, graph):
    require(graph['dependency_graph_complete'] and not graph['issues'] and
            not graph['execution_supported'] and not graph['initialization_schedule_available'],
            'Incomplete graph or overstated execution')
    model = graph['modeled_initialization']
    require(model['status'] == 'modeled' and model['scope'] == 'initial_version_fresh_dependents' and
            model['assumes_all_initializers_succeed'] and not model['gpu_execution_verified'],
            'Missing model scope or success assumption')
    saved = graph['nodes']; initial = trace['graph_before_first_node_initialize']
    nodes = {n['id']: n for n in initial}; cpp = {n['id']: n for n in saved}
    require(nodes and len(nodes) == len(initial) and len(cpp) == len(saved) and set(nodes) == set(cpp),
            'Missing or duplicate initial node')
    for ident, node in nodes.items():
        require(node['version'] == 0 and node['status'] == 1 and not node['dependents'],
                'Different initial readiness/dependent scope')
        require(cpp[ident]['status'] == 'recovered' and cpp[ident]['kind'] == node['kind'] and
                cpp[ident]['dependency_set'] == node['dependencies'], 'Different node dependencies')
        require(all(d in nodes for d in node['dependencies']), 'Missing dependency')
    calls = trace['node_initializations']
    require(calls and len({c['thread'] for c in calls}) == 1, 'Missing or concurrent native calls')
    require(model['order'] == [c['id'] for c in calls] and
            model['previous_statuses'] == [c['before_status'] for c in calls], 'Different native call sequence')
    status = {ident: 1 for ident in nodes}
    for call in calls:
        ident = call['id']
        require(ident in nodes and call['version'] == 0, 'Different callback identity/version')
        node = nodes[ident]
        require(call['kind'] == node['kind'] and call['dependencies'] == node['dependencies'],
                'Different callback metadata')
        require(call['initializer_rva'] == {'data':'0x9a6d0','resource':'0x9a6d0',
                'state':'0x9a710','erg':'0x9a6f0'}[node['kind']], 'Different native initializer')
        require(call['before_status'] == status[ident] and call['after_status'] == 2 and
                all(status[d] == 2 for d in node['dependencies']), 'Incorrect native readiness')
        status[ident] = 2
    require(all(s == 2 for s in status.values()), 'Uninitialized node remains')
    require([c['id'] for c in calls if c['kind'] == 'erg'] ==
            [o['event'] for o in trace['observations'] if o['kind'] == 'initialize_enter'],
            'ERG observer does not match node observer')
    return dict(nodes=len(nodes), calls=len(calls), by_kind=dict(Counter(c['kind'] for c in calls)))


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--case', nargs=3, action='append', required=True, metavar=('TRACE', 'CONTROL', 'CAPTURE'))
    for key in ['exe','qt-bin','out']: parser.add_argument('--'+key, type=Path, required=True)
    args = parser.parse_args(); args.out.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy(); env.pop('GPA_LOCAL_INJECT', None)
    env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + env.get('PATH', '')
    report = dict(schema='FloraGPA full initial schedule evidence 1', completed=False, passed=False,
                  traditional_list_execution_accepted=False, cases=[], exe_sha256=digest(args.exe),
                  source_sha256=digest(Path(__file__)))
    for index, paths in enumerate(args.case):
        observed, control, capture = map(Path, paths)
        trace, baseline = load(observed/'trace.json'), load(control/'trace.json')
        check_trace(trace); check_trace(baseline)
        require(not baseline['observers_enabled'], 'Control has observers')
        require(trace['player_sha256'] == baseline['player_sha256'] == PLAYER, 'Different player')
        require(digest(capture) == trace['capture_sha256'] == baseline['capture_sha256'], 'Different capture')
        image = original_rgba(observed)
        require(image and image == original_rgba(control), 'Observers changed original pixels')
        target = args.out / str(index)
        process = run([args.exe.resolve(), 'initialization-graph', capture.resolve(), '--out', target.resolve()],
                      args.out / (str(index)+'.log'), 180, env)
        require(process['exit_code'] == 0 and not process['timed_out'], 'C++ graph export failed')
        graph_path = target / 'initialization-graph.json'
        row = check_schedule(trace, load(graph_path))
        row.update(capture_sha256=digest(capture), original_control_pixels_equal=True,
                   evidence={str(p):digest(p) for p in [observed/'trace.json',control/'trace.json',graph_path]})
        report['cases'].append(row); write(args.out/'validation.json', report)
    report.update(completed=True, passed=True)
    write(args.out/'validation.json', report)
    print('Exact original/C++ initialization calls:', sum(c['calls'] for c in report['cases']))


if __name__ == '__main__': main()
