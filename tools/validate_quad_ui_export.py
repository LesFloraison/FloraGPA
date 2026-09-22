"""Verify Qt Quad ZIP members, frozen provenance and exact native/Python artifacts."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--real-native', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    checks = []
    for name, native in (('fixture.zip', args.artifacts / 'fixture-native'), ('quad.zip', args.real_native)):
        path = args.artifacts / name
        with zipfile.ZipFile(path) as archive:
            result = json.loads(archive.read('result.json'))
            key = result.pop('experiment_key')
            assert isinstance(key, str) and key, name
            assert result == json.loads((native / 'quad.json').read_text()), name
            members = ['result.json', *result['files'].values()]
            assert len(archive.namelist()) == len(members) == 7
            assert set(archive.namelist()) == set(members)
            assert archive.testzip() is None
            for member in result['files'].values():
                assert archive.read(member) == (native / member).read_bytes(), (name, member)
        checks.append(dict(archive=name, passed=True, entries=7, experiment_key=key,
                           sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
    report = dict(passed=True, checks=checks)
    (args.out / 'validation.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report))


if __name__ == '__main__':
    main()
