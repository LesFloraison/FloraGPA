"""Compare every initial node collector against unchanged original captures."""
import argparse
from collections import Counter
from pathlib import Path
import sys
from validate_compatibility_gate import digest, load, require
from validate_corpus import original_rgba, write
from validate_initial_cache_evidence import check_cache
from validate_original_init_traces import check_trace


def check_graph(trace, commands, entries, exported):
    result = check_cache(trace, commands, entries)
    require(exported['cache'] == commands['original_initialization_cache'], 'Different exported cache')
    require(exported['dependency_graph_complete'] and not exported['issues'] and
            not exported['initialization_schedule_available'] and not exported['execution_supported'],
            'Incomplete or overstated graph scope')
    nodes = {n['id']: n for n in exported['nodes']}
    original = {n['id']: n for n in trace['graph_before_first_erg_initialize']}
    require(len(nodes) == len(exported['nodes']) and set(nodes) == set(original), 'Missing/duplicate graph node')
    observations = [dict(n, id=n['event'], kind='erg') for n in trace['reference_collectors']]
    observations += trace['node_reference_collectors']
    require(len(observations) == len(nodes) and {n['id'] for n in observations} == set(nodes),
            'Missing/duplicate collector observation')
    factories = trace['node_factories']
    nonerg = {i for i, n in original.items() if n['kind'] != 'erg'}
    require(len(factories) == len(nonerg) and {n['id'] for n in factories} == nonerg,
            'Incomplete node factory coverage')
    for factory in factories:
        require(factory['present'] and factory['kind'] == nodes[factory['id']]['kind'] and
                factory['wire_type'] == entries[factory['id']]['type'], 'Different original node factory')
    types, kinds = Counter(), Counter()
    for observed in observations:
        ident = observed['id']; node = nodes[ident]; native = original[ident]
        require(node['kind'] == observed['kind'] == native['kind'] and
                node['type'] == observed['wire_type'] == entries[ident]['type'], 'Different node kind/type')
        require(node['status'] == 'recovered' and node['collector_rva'] == observed['collector_rva'],
                'Unrecovered or different node collector')
        require(node['collector_sequence'] == observed['references'], 'Different node order/duplicates')
        require(node['dependency_set'] == sorted(set(observed['references'])) == native['dependencies'],
                'Different node dependency set')
        require(all(d in nodes for d in native['dependencies']), 'Missing dependency identity')
        require(native['version'] == 0, 'Non-initial dependency version')
        if node['kind'] != 'erg':
            require(node['scope'] == 'original_initial_dependency_collection' and
                    not node['resource_objects_validated'], 'Overstated node validation')
        types[f"{node['kind']}:{node['type']:#x}"] += 1
        kinds[node['kind']] += 1
    return dict(**result, exact_nodes=len(nodes), kinds=dict(kinds), node_types=dict(types))


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--case', nargs=5, action='append', required=True,
                        metavar=('TRACE', 'CONTROL', 'COMMANDS', 'CAPTURE', 'GRAPH'))
    parser.add_argument('--reference-python', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args(); args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.reference_python.resolve(strict=True)))
    from frame import Frame
    report = dict(schema='FloraGPA initial graph evidence 1', completed=False, passed=False, cases=[])
    for paths in args.case:
        observed, control, commands, capture, graph = map(Path, paths)
        trace, baseline = load(observed/'trace.json'), load(control/'trace.json')
        check_trace(baseline)
        require(not baseline['observers_enabled'], 'Instrumented control')
        require(digest(capture) == trace['capture_sha256'] == baseline['capture_sha256'], 'Different capture')
        require(trace['player_sha256'] == baseline['player_sha256'], 'Different player')
        image = original_rgba(observed)
        require(image and image == original_rgba(control), 'Different original pixels')
        with Frame(capture) as frame:
            result = check_graph(trace, load(commands), {i: vars(e) for i,e in frame.entries.items()}, load(graph))
        result.update(capture_sha256=trace['capture_sha256'], image_equal=True,
                      evidence={str(p): digest(p) for p in [observed/'trace.json', control/'trace.json', commands, graph]})
        report['cases'].append(result); write(args.out/'validation.json', report)
    report.update(completed=True, passed=True, sources={p: digest(Path(__file__).parent/p) for p in
                  ['validate_initial_graph_evidence.py','validate_initial_cache_evidence.py',
                   'validate_original_reference_evidence.py','validate_original_init_traces.py']})
    write(args.out/'validation.json', report)
    print('Exact initial graph nodes:', sum(c['exact_nodes'] for c in report['cases']))


if __name__ == '__main__': main()
