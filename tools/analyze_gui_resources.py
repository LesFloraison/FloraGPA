"""Audit accepted GUI snapshots and summarize correlations, not allocation owners.

Usage: python tools/analyze_gui_resources.py <soak-output-directory>
Output is JSON on stdout. Input evidence is never modified.
"""
import argparse
from collections import Counter
import json
from pathlib import Path

from validate_recovery_soak import digest, validate_gui_resources


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def difference(before, after):
    return {key: after.get(key, 0) - before.get(key, 0)
            for key in sorted(before.keys() | after.keys())
            if after.get(key, 0) != before.get(key, 0)}


def summarize(root):
    report = load(root / 'validation.json')
    if not (report.get('completed') is True and report.get('passed') is True and
            report.get('gui_resources') is True and report.get('qt_platform') == 'windows'):
        raise ValueError('Expected completed, accepted native GUI observations')
    journal_path = root / 'journal.json'
    if digest(journal_path) != report['journal_sha256']:
        raise ValueError('Journal identity differs from accepted run')
    journal = load(journal_path)
    if journal.get('completed') is not True or journal.get('phase') != 'complete':
        raise ValueError('Incomplete GUI journal')
    actual = validate_gui_resources(root / 'gui-resources', journal)
    if actual != report['gui_resource_files']:
        raise ValueError('GUI snapshot identity differs from accepted run')
    names = ['before_window.json']
    names += [f'cycle-{index:06d}.json' for index in range(1, len(journal['observations']) + 1)]
    names += [f'control-{key}.json' for key in
              ['before', 'after_test_history_clear', 'after_log_clear', 'after_pixmap_cache_clear']]
    names += ['after_window_destroy.json']
    rows, previous = [], None
    for name in names:
        snapshot = load(root / 'gui-resources' / name)
        row = dict(snapshot=name, counts=snapshot['after'],
                   during_probe_delta=difference(snapshot['before'], snapshot['after']))
        for key in ['native_top_windows', 'qt_top_widgets', 'qt_windows']:
            row[key] = dict(sorted(Counter(item['class'] for item in snapshot[key]).items()))
        # Paths are metadata, not evidence of which module allocated a resource.
        modules = {item['path'].casefold(): item['path'] for item in snapshot['modules']}
        if len(modules) != len(snapshot['modules']):
            raise ValueError('Duplicate module paths in GUI snapshot')
        row['module_count'] = len(modules)
        if previous is not None:
            row['since_previous'] = dict(counts=difference(previous['counts'], row['counts']),
                modules_added=[modules[key] for key in sorted(modules.keys() - previous['modules'].keys())],
                modules_removed=[previous['modules'][key] for key in sorted(previous['modules'].keys() - modules.keys())])
            for key in ['native_top_windows', 'qt_top_widgets', 'qt_windows']:
                row['since_previous'][key] = difference(previous[key], row[key])
        previous = row | dict(modules=modules)
        rows.append(row)
    return dict(schema='FloraGPA GUI resource analysis 1', observations=rows,
                journal_sha256=report['journal_sha256'], snapshot_files=actual,
                scope='Accepted current-process sequential snapshots. Module/window/count correlations '
                      'are not allocation ownership or leak proof. QApplication remains alive after '
                      'MainWindow destruction. No observation of child/message-only windows, '
                      'data-file modules or resources after process exit.')


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.directory), indent=2))


if __name__ == '__main__':
    main()
