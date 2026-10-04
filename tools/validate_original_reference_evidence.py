"""Audit native collector observations against C++ metadata and original graphs."""
import argparse
from collections import Counter
from pathlib import Path
from validate_compatibility_gate import digest, load, require
from validate_corpus import original_rgba, write
from validate_original_init_traces import check_trace


def check_references(trace, commands):
    check_trace(trace)
    rows = {r['id']: r for r in commands['commands']}
    require(len(rows) == len(commands['commands']), 'Duplicate command ID')
    graph = {n['id']: n for n in trace['graph_before_first_erg_initialize'] if n['kind'] == 'erg'}
    observed = trace['reference_collectors']
    require(len(observed) == len(graph) and {o['event'] for o in observed} == set(graph),
            'Missing or duplicate reference observations')
    recovered, gaps, types = 0, Counter(), Counter()
    for observation in observed:
        event = observation['event']
        row = rows[event]
        require(row['type'] == observation['wire_type'], 'Different ERG wire type')
        references = observation['references']
        require(sorted(set(references)) == graph[event]['dependencies'], 'Native graph/collector mismatch')
        info = row['original_initialization']
        require(not info['execution_dependencies_complete'] and not info['registration_sequence_available'],
                'Overstated execution scope')
        if info['status'] == 'recovered':
            require(row['status'] == 'decoded', 'Recovered incomplete record')
            require(info['collector_rva'] == observation['collector_rva'], 'Different native collector')
            require(info['collector_sequence'] == references, 'Different collector order/duplicates')
            require(info['dependency_set'] == graph[event]['dependencies'], 'Different dependency set')
            recovered += 1
            types[hex(row['type'])] += 1
        else:
            require(info['status'] == 'unrecovered' and row['type'] == 0x25e and
                    'collector_sequence' not in info and info.get('reason'), 'Unexpected recovery gap')
            gaps[hex(row['type'])] += 1
    return dict(observed=len(observed), recovered=recovered, unrecovered=dict(gaps), types=dict(types))


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--case', nargs=3, action='append', required=True,
                        metavar=('TRACE_DIR', 'CONTROL_DIR', 'COMMANDS'))
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    report = dict(schema='FloraGPA original reference comparison 1',completed=False,passed=False,cases=[])
    for observed, control, commands in args.case:
        observed, control, commands = Path(observed), Path(control), Path(commands)
        trace = load(observed/'trace.json')
        reference = load(control/'trace.json')
        check_trace(reference)
        require(not reference['observers_enabled'], 'Control contains observers')
        require(trace['capture_sha256'] == reference['capture_sha256'], 'Different control capture')
        require(trace['player_sha256'] == reference['player_sha256'], 'Different original player')
        image, baseline = original_rgba(observed), original_rgba(control)
        require(image and image == baseline, 'Observers changed original output')
        case = check_references(trace,load(commands))
        case.update(trace=str(observed),control=str(control),commands=str(commands),
                    trace_sha256=digest(observed/'trace.json'),commands_sha256=digest(commands),
                    control_sha256=digest(control/'trace.json'),capture_sha256=trace['capture_sha256'],
                    image_equal=True,rgba_bytes=len(image))
        report['cases'].append(case)
        write(args.out/'validation.json',report)
    report.update(completed=True,passed=True,source_sha256=digest(Path(__file__)))
    write(args.out/'validation.json',report)
    print('Reference checks passed:',sum(c['recovered'] for c in report['cases']),
          'recovered /',sum(c['observed'] for c in report['cases']),'observed')


if __name__ == '__main__':
    main()
