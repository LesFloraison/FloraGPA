"""Development-only, reproducible inventory. Capture binaries remain outside Git."""
import argparse
import hashlib
import json
import re
from pathlib import Path

GAMES = {
    'GF2_Exilium_2026_03_03__00_19_35.gpa_frame': ('gf2', '2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1'),
    'bf1_2026_01_21__16_53_05.gpa_frame': ('bf1', '1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6'),
    'helldivers2_2026_04_02__18_02_58.gpa_frame': ('helldivers', None),
}

def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def catalog(root):
    files = [root / name for name in GAMES if (root / name).is_file()]
    files += sorted((root / 'analysis/capture_samples').rglob('*.gpa_frame'))
    cases = []
    for path in files:
        relative = path.relative_to(root).as_posix()
        sha = digest(path)
        game = GAMES.get(path.name) if path.parent == root else None
        family = game[0] if game else path.relative_to(root / 'analysis/capture_samples').parts[0]
        links = []
        folder = path.parent
        while folder != root:
            for evidence in sorted(folder.glob('*manifest*.json')):
                text = evidence.read_text(encoding='utf-8-sig')
                links.append({'path': evidence.relative_to(root).as_posix(), 'sha256': digest(evidence),
                              'contains_capture_hash': sha in text})
            folder = folder.parent
        identity = family + '__' + path.stem
        if not game: identity += '__' + hashlib.sha256(relative.encode('utf-8')).hexdigest()[:8]
        cases.append({'id': re.sub(r'[^A-Za-z0-9_-]', '-', identity),
                      'path': relative, 'sha256': sha, 'bytes': path.stat().st_size,
                      'family': family, 'origin': 'user_supplied_game_capture' if game else 'local_research_fixture',
                      'provenance_note': 'Linked manifests are evidence pointers; modified fixtures are not certified as original captures.',
                      'source_manifests': links, 'device_scope': 'Record the actual replay adapter; fixture provenance may constrain interpretation.',
                      'reference_rgba_sha256': game[1] if game else None,
                      'comparison_policy': 'exact_golden' if game and game[1] else 'known_variable' if family == 'helldivers' else 'observe',
                      'boundaries': [{'resource': 18325, 'event': 18344, 'expect_stable': True},
                                     {'resource': 18325, 'event': 18401, 'expect_stable': False}]
                                    if family == 'helldivers' and sha == '97915a80c7617e9a67068a5d85b87f1b2a7883faa9831428084ebd7270f37139' else [],
                      'controls': [{'disable_event': 18401, 'repeat': 3,
                                    'expected_rgba_sha256': '22f2d66bdbca7a3b1aa868c5a68e39ebc7669e070596eec6b8f0e6cc922e2605'}]
                                  if family == 'helldivers' and sha == '97915a80c7617e9a67068a5d85b87f1b2a7883faa9831428084ebd7270f37139' else []})
    return {'schema': 'FloraGPA compatibility corpus 1', 'profile': 'GPA 2025 R1 legacy DX11 / IGPA v3',
            'root_parameter': '--captures-root', 'cases': cases}

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    result = catalog(args.root.resolve())
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f"Cataloged {len(result['cases'])} captures; no binary files copied")
