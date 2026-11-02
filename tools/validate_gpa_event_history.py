"""Compare native GPA event diagnostics with an original-player probe report.

The caller must select the texture underlying the original probe's RTV. This
validator compares observations, not resource/view numeric identifiers.
"""
import argparse
import json
from pathlib import Path


def compare(native, original):
    assert original['completed'] and native['ok'], 'incomplete report'
    context, data = native['context'], native['data']
    assert context['frame_sha256'] == original['capture_sha256'], 'capture changed'
    assert context['adapter']['vendor'] == original['vendor'], 'adapter vendor mismatch'
    assert data['kind'] == 'gpa_event_history' and data['schema'] == 1
    for key in ('x', 'y', 'mip', 'layer'):
        assert data['selection'][key] == original['selection'][key], key
    rows = data['records']
    for row in rows:
        assert row['record_kind'] == 'gpa_event_diagnostic'
        assert row['pixel']['available'], 'unavailable pixel'
    observations = [result for request in original['requests'] for result in request['results']]
    assert observations, 'missing original callbacks'
    for observed in observations:
        assert [r['event'] for r in rows] == observed['events'], 'event sequence mismatch'
        assert [r['diagnostic_status'] for r in rows] == observed['raw_status'], 'status mismatch'
        assert [r['diagnostic_count'] for r in rows] == observed['diagnostic_words'], 'count mismatch'
        assert ''.join(r['pixel']['bytes'] for r in rows) == observed['pixel_bytes'], 'pixel mismatch'
    return {'ok': True, 'events': len(rows), 'original_callbacks': len(observations),
            'frame_sha256': context['frame_sha256'], 'adapter': context['adapter'],
            'limits': 'Same-vendor fixture comparison; resource-to-RTV mapping is supplied by the caller.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-result', required=True, type=Path)
    parser.add_argument('--original', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    result = compare(json.loads(args.native_result.read_text(encoding='utf-8')),
                     json.loads(args.original.read_text(encoding='utf-8')))
    args.out.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result))
