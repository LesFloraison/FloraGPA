"""Run original bridge acceptance tests against the migrated DLL, without editing the reference."""
import argparse
from pathlib import Path
import runpy
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--bridge', type=Path, required=True)
    parser.add_argument('--mode', choices=('samples', 'recorded'), required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    reference, bridge = args.reference.resolve(), args.bridge.resolve()
    script = reference / 'tools' / f'validate_md_{args.mode}.py'
    namespace = runpy.run_path(str(script))
    sys.argv = [str(script), '--out', str(args.out.resolve())]
    if args.mode == 'samples':
        sys.argv.extend(['--bridge', str(bridge), '--reuse'])
    else:
        original = namespace['Metrics']
        primary = reference / 'output/metrics-discovery-recorded-build/Release/flora_metrics.dll'

        class PortedMetrics(original):
            def __init__(self, device, selected_bridge=None):
                if selected_bridge is not None and Path(selected_bridge).resolve() == primary:
                    selected_bridge = bridge
                super().__init__(device, selected_bridge)

        # Only redirect the primary bridge; retain the historical compatibility DLL.
        namespace['main'].__globals__['Metrics'] = PortedMetrics
    namespace['main']()


if __name__ == '__main__':
    main()
