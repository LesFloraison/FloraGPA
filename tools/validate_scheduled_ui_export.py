"""Development-only verification of the native Qt scheduled-metric ZIP export."""
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
    from md_iteration_results import load_scheduled_result

    def read(path):
        return json.loads(path.read_text(encoding='utf-8-sig'))

    archive = args.artifacts / 'scheduled-metrics.zip'
    extracted = args.out / 'extracted'
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None, 'ZIP CRC failure'
        names = z.namelist()
        assert len(names) == len(set(n.casefold() for n in names)), 'Duplicate ZIP member'
        for name in names:
            path = PurePosixPath(name)
            assert not path.is_absolute() and '..' not in path.parts and ':' not in name and '\\' not in name
            assert path.suffix.lower() not in ('.dll', '.exe')
        z.extractall(extracted)

    profile = read(extracted / 'scheduled-profile.json')
    ui = read(args.artifacts / 'ui-result.json')
    publisher = ui.pop('publisher_result')
    assert ui == profile, 'Saved profile differs from visible result'
    assert load_scheduled_result(extracted, profile) == publisher, 'Python reader disagrees'
    assert profile['weight_source'] == 'caller_cache'
    assert read(extracted / 'input-weights.json') == profile['supplied_weights'] == [0, 2, 4]
    process = read(extracted / 'process-tree.json')
    assert process['assigned_to_job'] is True and process['kill_on_job_close'] is True
    log = (extracted / 'worker.log').read_text(encoding='utf-8')
    assert '"replay"' in log and '"ranges"' in log

    module_reports = []
    for path in extracted.rglob('*.json'):
        value = read(path)
        if not isinstance(value, dict) or 'loaded_modules' not in value:
            continue
        modules = [Path(p) for p in value['loaded_modules']]
        assert modules, str(path)
        assert not any(p.name.lower().startswith(('python', 'gpa-', 'gpa_', 'tk8', 'tcl8'))
                       or p.name.lower() == 'renderdoc.dll' for p in modules), str(path)
        qt = [p for p in modules if p.name.lower().startswith('qt6')]
        assert qt and all(p.parent.resolve() == args.package.resolve() for p in qt), str(path)
        module_reports.append(str(path.relative_to(extracted)))
    assert module_reports, 'No worker module evidence'
    result = dict(passed=True, archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
                  members=len(names), raw_reports=len(profile['records']), cells=len(profile['metrics']),
                  module_reports=module_reports, publisher_equal=True, profile_equal=True,
                  process_assigned=True, cached_weights=True)
    (args.out / 'validation.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
