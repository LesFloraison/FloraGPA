"""Development-only RenderDoc embedded-Python event dump for the native oracle.

Run through qrenderdoc --python with sys.argv = [script, reference, capture, out].
FloraGPA never imports or launches this file.
"""
import json
from pathlib import Path
import sys
import renderdoc as rd

reference, capture_path, output = map(Path, sys.argv[1:])
sys.path.insert(0, str(reference / 'standalone'))
from rdc_cpu_history import native_events
from rdc_events import index

capture = controller = None
try:
    capture = rd.OpenCaptureFile()
    status = capture.OpenFile(str(capture_path), '', None)
    if status != rd.ResultCode.Succeeded:
        raise RuntimeError(str(status))
    status, controller = capture.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded:
        raise RuntimeError(str(status))
    capture.Shutdown()
    capture = None
    roots = controller.GetRootActions()
    native = native_events(roots, controller.GetStructuredFile().chunks)
    def convert(n):
        return dict(eventId=n.eventId, customName=n.customName, flags=int(n.flags), flags_text=str(n.flags),
            numIndices=n.numIndices, numInstances=n.numInstances,
            events=[dict(eventId=e.eventId) for e in n.events], children=[convert(x) for x in n.children])
    result = dict(roots=[convert(x) for x in roots],
                  native={eid: dict(name=value['name']) for eid, value in native.items()},
                  resources=[dict(id=str(r.resourceId), name=r.name) for r in controller.GetResources()])
    actions, _, selected, reverse, commands = index(rd, roots, native)
    result['reference'] = dict(actions=actions, gpa_event_map=selected, gpa_command_map=commands, reverse=reverse)
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
finally:
    if controller is not None:
        controller.Shutdown()
    if capture is not None:
        capture.Shutdown()
