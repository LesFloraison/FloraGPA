"""Development-only original-reader checks for Qt uniform metric exports."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import sys
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('reference', 'artifacts', 'package', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.reference / 'standalone'))
    from metric_values import summarize_records
    from metric_passes import profile_matrix
    from metric_planner import requested_results
    from md_publisher_values import load_publisher_result, publisher_analysis

    def read(path):
        return json.loads(path.read_text(encoding='utf-8-sig'))

    results = []
    for mode in ('sets', 'request', 'request-multipass'):
        archive = args.artifacts / f'uniform-{mode}.zip'
        extracted = args.out / mode
        with zipfile.ZipFile(archive) as z:
            assert z.testzip() is None
            names = z.namelist()
            assert len(names) == len(set(n.casefold() for n in names))
            for name in names:
                p = PurePosixPath(name)
                assert not p.is_absolute() and '..' not in p.parts and ':' not in name and '\\' not in name
                assert p.suffix.lower() not in ('.dll', '.exe')
            z.extractall(extracted)
        profile = read(extracted / 'profile.json')
        ui = read(args.artifacts / f'{mode}-result.json')
        publisher = ui.pop('publisher_result')
        assert ui == profile
        assert load_publisher_result(extracted, profile) == publisher
        assert profile_matrix(profile) == read(extracted / 'metric-iterations.json')['sets']
        assert summarize_records(profile) == read(extracted / 'sample-statistics.json')
        converted = publisher_analysis(profile, publisher)
        assert converted['statistics'] == read(extracted / 'publisher-statistics.json')
        if mode.startswith('request'):
            assert requested_results(profile, profile['metric_request']) == read(extracted / 'requested-metrics.json')
            assert converted['requested'] == read(extracted / 'publisher-requested-metrics.json')
        process = read(extracted / 'process-tree.json')
        assert process['assigned_to_job'] is True and process['kill_on_job_close'] is True
        assert '"set"' in (extracted / 'worker.log').read_text(encoding='utf-8')
        for row in profile['records']:
            assert hashlib.sha256((extracted / row['raw_report']).read_bytes()).hexdigest() == row['raw_sha256']
        modules = []
        for path in extracted.rglob('*.json'):
            value = read(path)
            if not isinstance(value, dict) or 'loaded_modules' not in value:
                continue
            loaded = [Path(p) for p in value['loaded_modules']]
            assert loaded
            assert not any(p.name.lower().startswith(('python', 'gpa-', 'gpa_', 'tk8', 'tcl8')) or p.name.lower() == 'renderdoc.dll' for p in loaded)
            qt = [p for p in loaded if p.name.lower().startswith('qt6')]
            assert qt and all(p.parent.resolve() == args.package.resolve() for p in qt)
            modules.append(str(path.relative_to(extracted)))
        assert modules
        results.append(dict(mode=mode, archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(), members=len(names),
                            records=len(profile['records']), module_reports=modules, original_reader_equal=True))
    result = dict(passed=True, cases=results)
    (args.out / 'validation.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
