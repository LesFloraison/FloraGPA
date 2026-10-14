"""Development-only ISA export through the installed GPA Plugin API."""
import hashlib
import json
import os
import time
import traceback
import plugin_api


def normalized(text):
    return text.rstrip('\0').replace('\r\n', '\n')


def digest(text):
    return hashlib.sha256(text.encode('utf-8')).hexdigest()


def run(output_directory: "Existing new audit directory"):
    report = {'completed': False, 'acceptance_established': False, 'requests': []}
    report_path = os.path.join(output_directory, 'report.json')
    if os.path.exists(report_path):
        raise RuntimeError('Audit report already exists')

    def save():
        with open(report_path, 'w', encoding='utf-8') as stream:
            json.dump(report, stream, indent=2, ensure_ascii=False)

    try:
        with open(os.path.join(output_directory, 'request.json'), encoding='utf-8') as stream:
            request = json.load(stream)
        programs = plugin_api.get_resources_accessor().get_programs()
        selected = []
        for program in programs:
            description = program.get_description()
            if all(digest(normalized(description.get(stage, {}).get('dxbc', ''))) == expected
                   for stage, expected in request['shader_assembly_sha256'].items()):
                selected.append(program)
        report['matching_program_ids'] = [p.get_description()['id'] for p in selected]
        if len(selected) != request['expected_matching_programs']:
            raise RuntimeError('Unexpected number of programs with exact VS/PS assembly')
        save()
        for program in selected:
            description = program.get_description()
            for stage in ('pixel', 'vertex'):
                item = {'program_id': description['id'], 'stage': stage, 'completed': False}
                report['requests'].append(item)
                save()
                started = time.monotonic()
                try:
                    source = program.get_il_source(stage, 'isa', timeout_ms=30000)
                    item['return_type'] = type(source).__name__
                    item['source'] = source
                    item['nonempty_text'] = isinstance(source, str) and bool(source.strip())
                    if isinstance(source, str):
                        item['source_sha256'] = digest(source)
                        item['utf8_bytes'] = len(source.encode('utf-8'))
                    item['completed'] = True
                except Exception:
                    item['error'] = traceback.format_exc()
                item['elapsed_seconds'] = time.monotonic() - started
                save()
        report['completed'] = True
        report['all_isa_sources_nonempty'] = all(r.get('nonempty_text', False) for r in report['requests'])
    except Exception:
        report['error'] = traceback.format_exc()
    save()
    return []


def desc():
    return {'name': 'Flora Shader Audit',
            'description': 'Development-only original GPA ISA export.',
            'apis': [], 'applicabilities': ['Apilog', 'Resources'],
            'plugin_api_version': '1.2'}
