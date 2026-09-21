"""Run preserved CPU/GPU oracles with native lowering and native compilation.

Only the development test driver uses Python. No product process imports it.
The original validators' expected CPU values and D3D11 readback code are unchanged.
"""
import argparse
from contextlib import ExitStack
import importlib
import json
import os
from pathlib import Path
import subprocess
import sys
from unittest.mock import patch


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--reference', type=Path, required=True)
    p.add_argument('--probe', type=Path, required=True)
    p.add_argument('--qt-bin', type=Path)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path[:0] = [str(args.reference/'standalone'), str(args.reference/'tools')]
    from hlsl_recover import Lowerer as ReferenceLowerer
    env = dict(os.environ)
    if args.qt_bin:
        env['PATH'] = str(args.qt_bin.resolve())+os.pathsep+env.get('PATH', '')
    count = 0

    def native(job):
        nonlocal count
        count += 1
        request = args.out/f'job-{count}.json'
        response = args.out/f'result-{count}.json'
        request.write_text(json.dumps([job], ensure_ascii=True), encoding='utf-8')
        subprocess.run([str(args.probe.resolve()), '--probe', str(request.resolve()), str(response.resolve())],
                       env=env, check=True, timeout=90)
        result = json.loads(response.read_text('utf-8'))[0]
        if not result['ok']:
            raise ValueError(result['error'])
        return result['result']

    class NativeLowerer:
        def __init__(self, bytecode):
            oracle = ReferenceLowerer(bytecode)
            self.info, self.lines = oracle.info, oracle.lines

        def translate(self):
            return native(dict(action='translate', info=self.info, assembly='\n'.join(self.lines)))

    def recover(bytecode):
        return native(dict(action='recover', bytecode=bytecode.hex()))

    def compile_hlsl(source, profile, entry='main', name='edited.hlsl', optimization='auto'):
        if isinstance(source, bytes):
            source = source.decode('utf-8')
        result = native(dict(action='compile', source=source, profile=profile, entry=entry,
                             name=name, optimization=optimization))
        return bytes.fromhex(result['bytecode']), result['diagnostics']

    names = ['validate_hlsl_integer', 'validate_hlsl_memory', 'validate_hlsl_graphics_gpu',
             'validate_hlsl_geometry', 'validate_hlsl_gather_counter', 'validate_hlsl_shadow_cube']
    modules = [importlib.import_module(name) for name in names]
    results = []
    with ExitStack() as stack:
        for module in modules:
            for name, value in [('recover', recover), ('Lowerer', NativeLowerer), ('compile_hlsl', compile_hlsl)]:
                if hasattr(module, name):
                    stack.enter_context(patch.object(module, name, value))
        for module in modules:
            target = args.out/module.__name__
            with patch.object(sys, 'argv', [module.__name__, '--out', str(target.resolve())]):
                module.main()
            report = json.loads((target/'validation.json').read_text('utf-8'))
            results.append(dict(suite=module.__name__, passed=True, report=report))
            (args.out/'validation.json').write_text(json.dumps(dict(passed=len(results)==len(modules), native_calls=count, results=results), indent=2), encoding='utf-8')
            print(module.__name__, 'PASS', flush=True)
    print(json.dumps(dict(passed=True, suites=len(results), native_calls=count)))


if __name__ == '__main__':
    main()
