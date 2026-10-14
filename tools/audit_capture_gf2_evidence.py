"""Read-only development audit of retained GF2 captures and dual-player outputs.

No GPU execution, GPA loading, changes to evidence, or acceptance by preview.
The input layout is the documented two-launch/three-frame GF2 experiment.
This checks execution evidence; scene and detailed GUI review remain separate.
"""
import argparse
import hashlib
import json
import mmap
from pathlib import Path
import struct
import sys

PLAYER_SHA = '39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b'
GF2 = Path('D:/Program Files/GF2Exilium/GF2 Game/GF2_Exilium.exe')


def require(ok, reason):
    if not ok:
        raise ValueError(reason)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def reference(path):
    """Bounded extraction of the encoder's dedicated final RGBA8 resource."""
    with path.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        require(len(data) >= 0x128 and data[:4] == b'IGPA', 'Truncated or non-IGPA frame')
        require(struct.unpack_from('<II', data, 4) == (0x128, 3), 'Unverified container version')
        count = struct.unpack_from('<I', data, 12)[0]
        table = struct.unpack_from('<Q', data, 244)[0]
        require(0 < count <= 1_000_000 and table >= 0x128 and table <= len(data) - count * 24,
                'Invalid container index bounds')
        entries, candidates = {}, []
        for i in range(count):
            ident, at, size, flags, category, kind = struct.unpack_from('<QQIBBH', data, table + 24*i)
            require(ident not in entries and not flags and at >= 0x128 and at <= table - size,
                    'Duplicate, compressed or out-of-bounds record')
            entries[ident] = (at, size, category, kind)
            if (category, kind, size) == (5, 0x87, 68) and struct.unpack_from('<QQ', data, at) == (2**64-2, 3):
                candidates.append(at)
        require(len(candidates) == 1, 'Dedicated final reference missing or ambiguous')
        at = candidates[0]
        width, height, mips, layers, fmt, samples, quality = struct.unpack_from('<7I', data, at+16)
        require(0 < width*height <= 64_000_000 and (mips, layers, fmt, samples, quality) == (1, 1, 28, 1, 0),
                'Unverified reference resource layout')
        ident = struct.unpack_from('<Q', data, at+60)[0]
        require(ident in entries, 'Reference blob missing')
        start, size, category, kind = entries[ident]
        require((category, kind) == (9, 1) and size == 4 + width*height*4 and
                struct.unpack_from('<I', data, start)[0] == size-4, 'Reference blob length mismatch')
        return width, height, data[start+4:start+size]


def tga(path):
    data = path.read_bytes()
    require(len(data) >= 18, 'Truncated TGA header')
    ident, cmap, kind = data[:3]
    width, height, depth, flags = struct.unpack_from('<HHBB', data, 12)
    require(cmap == 0 and kind == 2 and depth == 32 and not flags & 0xD0 and
            0 < width*height <= 64_000_000, 'Unverified TGA layout')
    raw = data[18+ident:18+ident+width*height*4]
    require(len(raw) == width*height*4, 'Truncated TGA pixels')
    if not flags & 0x20:
        stride = width*4
        raw = b''.join(raw[y*stride:(y+1)*stride] for y in reversed(range(height)))
    rgba = bytearray(raw)
    rgba[0::4], rgba[2::4] = raw[2::4], raw[0::4]
    return width, height, bytes(rgba)


def new_image(folder):
    paths = list(folder.glob('framebuffer_new*.tga'))
    require(len(paths) == 1, 'Missing or ambiguous original executed image')
    return tga(paths[0])


def differences(a, b):
    require(len(a) == len(b) and len(a) % 4 == 0, 'RGBA extent mismatch')
    if a == b:
        return dict(pixels=0, channels=0, maximum=0)
    pixels, channels, maximum, last_pixel = 0, 0, 0, -1
    for index, (x, y) in enumerate(zip(a, b)):
        if x != y:
            channels += 1
            maximum = max(maximum, abs(x-y))
            if index//4 != last_pixel:
                pixels += 1
                last_pixel = index//4
    return dict(pixels=pixels, channels=channels, maximum=maximum)


class Audit:
    def __init__(self, root, runtime, controller, replay):
        self.root = root
        self.checks, self.frames = [], []
        self.expected = dict(runtime_sha256=digest(runtime), controller_sha256=digest(controller),
                             replay_sha256=digest(replay))
        self.inputs = {str(p): digest(p) for p in (runtime, controller, replay)}

    def check(self, name, ok, **details):
        self.checks.append(dict(name=name, passed=bool(ok), **details))

    def record(self, path):
        self.inputs[str(path)] = digest(path)
        return path

    def json(self, path):
        return read(self.record(path))

    def player(self, directory, original, negative=False, capture=None):
        report = self.json(directory / ('result.json' if original else 'report.json'))
        if original:
            audit = report['audit']
            ok = report['closed'] and report['open_status'] == report['playback_status'] == 0 and not report['callbacks']
            ok &= report['player_sha256'] == PLAYER_SHA and audit['negative_control_suppress_draws'] is negative
            adapters = audit['actual_context_adapters']
            ok &= bool(adapters) and all(a['vendor_id'] == 0x8086 for a in adapters)
            ok &= audit['d3d11_calls'].get('suppressed_DrawIndexed' if negative else 'DrawIndexed', 0) > 0
            self.check(str(directory.relative_to(self.root))+'/execution', ok)
            output = new_image(directory)
            for path in directory.glob('framebuffer_new*.tga'):
                self.record(path)
            return output
        counts = report['counts']
        draws = sum(value for name, value in counts.items() if name.startswith('Draw'))
        self.check(str(directory.relative_to(self.root))+'/execution', report['completed'] and
                   report['reference_pixels_used'] is False and report['adapter'] == 'Intel(R) Iris(R) Xe Graphics' and
                   counts.get('Present', 0) == 1 and (draws == 0 if negative else counts.get('DrawIndexed', 0) > 0) and
                   capture is not None and Path(report['capture']).resolve() == capture.resolve())
        return report['width'], report['height'], self.record(directory/'frame.rgba').read_bytes()

    def run(self):
        identity = self.json(self.root/'identity.json')
        for name, expected in self.expected.items():
            self.check('current/'+name, identity[name] == expected, recorded=identity[name], requested=expected)
        sessions, processes, files = set(), set(), set()
        for launch in (1, 2):
            root = self.root/f'launch-{launch}'
            session = self.json(root/'session/session.json')
            sessions.add(session['session_id'])
            processes.add((session['pid'], session['process_created']))
            self.check(f'{launch}/target', Path(session['executable']).resolve() == GF2.resolve())
            runtime = self.record(Path(session['runtime']))
            self.check(f'{launch}/runtime-file', self.inputs[str(runtime)] == identity['runtime_sha256'])
            disconnected, exited = self.json(root/'disconnect.json'), self.json(root/'exited.json')
            self.check(f'{launch}/finished-session', disconnected['state'] == 'Disconnected' and
                       disconnected['session_id'] == session['session_id'] and disconnected['pid'] == session['pid'] and
                       exited['state'] == 'Exited' and not list((root/'session').glob('*.partial')))
            listed = {p.resolve() for p in (root/'session').glob('*.gpa_frame')}
            actual, ordinals, chains = set(), [], set()
            for index in range(3):
                status = self.json(root/f'status-{index}.json')
                progress = status['capture_progress']
                file = Path(progress['file']).resolve()
                actual.add(file)
                files.add(file)
                ordinals.append(progress['present_ordinal'])
                chains.add(progress['swap_chain'])
                label = f'{launch}/{index}'
                modules = [p.replace('\\', '/').lower() for p in status['loaded_modules']]
                native = [p for p in modules if p.endswith('/floracaptureruntime.dll')]
                forbidden = ('qt5', 'qt6', 'python', 'florareplay', 'floracore', 'gpa_', 'gpa-')
                self.check(label+'/capture-identity', progress['state'] == 'Completed' and
                           status['session_id'] == session['session_id'] and status['pid'] == session['pid'] and
                           not status['objects']['interface_gaps'] and not status['objects']['lost_diagnostics'])
                self.check(label+'/runtime-independence', len(native) == 1 and Path(native[0]).resolve() == Path(session['runtime']).resolve() and
                           all(not Path(p).name.startswith(forbidden) and Path(p).name not in ('renderdoc.dll', 'dx11_player.dll', 'frameanalyzer.exe') for p in modules))
                expected = reference(file)
                row = dict(launch=launch, index=index, file=str(file), sha256=digest(file),
                           reference_sha256=hashlib.sha256(expected[2]).hexdigest(), comparisons=[])
                for repeat in range(3):
                    original_dir = root/f'original-{index}-{repeat}'
                    self.check(label+f'/{repeat}/original-reference', tga(self.record(original_dir/'framebuffer_orig.tga')) == expected)
                    for name in ('flora', 'original'):
                        output = self.player(root/f'{name}-{index}-{repeat}', name == 'original', capture=file)
                        require(output[:2] == expected[:2], 'Executed image dimensions differ from capture')
                        delta = differences(output[2], expected[2])
                        self.check(label+f'/{repeat}/{name}/exact', delta['pixels'] == 0, **delta)
                        row['comparisons'].append(dict(repeat=repeat, player=name, **delta))
                negative = [self.player(root/f'negative-{index}-{name}', name == 'original', True, file) for name in ('flora', 'original')]
                self.check(label+'/negative-control', negative[0] == negative[1] and negative[0][:2] == expected[:2] and negative[0][2] != expected[2])
                self.frames.append(row)
            self.check(f'{launch}/three-distinct-frames', actual == listed and len(actual) == 3 and ordinals == sorted(set(ordinals)))
            after = self.json(root/'after.json')
            self.check(f'{launch}/continued-presenting', len(chains) == 1 and
                       after['session_id'] == session['session_id'] and after['pid'] == session['pid'] and
                       any(s['id'] in chains and s['successful_presents'] > ordinals[-1] for s in after['swap_chains']['active']))
        self.check('two-distinct-launches-six-files', len(sessions) == len(processes) == 2 and len(files) == 6)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('evidence', 'runtime', 'controller', 'replay', 'out'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    require(not args.out.exists(), 'Output must be a new file')
    audit = Audit(args.evidence.resolve(strict=True), args.runtime.resolve(strict=True),
                  args.controller.resolve(strict=True), args.replay.resolve(strict=True))
    error = None
    try:
        audit.run()
    except (OSError, ValueError, KeyError, TypeError, struct.error) as exc:
        error = f'{type(exc).__name__}: {exc}'
    passed = error is None and bool(audit.checks) and all(c['passed'] for c in audit.checks)
    report = dict(schema=1, scope=__doc__, passed=passed, formal_acceptance_established=False,
                  additional_required_review=['Two scenes and capture/final-backbuffer correspondence',
                                              'Per-file event, shader, resource and pipeline inspection in both tools',
                                              'Complete packaging and supported-scope audit'],
                  error=error, expected_identity=audit.expected, input_hashes=audit.inputs,
                  checks=audit.checks, frames=audit.frames, tool_sha256=digest(Path(__file__)))
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2)
        stream.write('\n')
    print(json.dumps(dict(passed=passed, frames=len(audit.frames), checks=len(audit.checks),
                          failed=[c['name'] for c in audit.checks if not c['passed']], error=error)))
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
