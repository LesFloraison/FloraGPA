"""Compare a Qt-exported Coverage ZIP with an original Python coverage result."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import zipfile


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--reference',type=Path,required=True)
    p.add_argument('--expected',type=Path,required=True)
    p.add_argument('--archive',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    args=p.parse_args()
    args.out.mkdir(parents=True,exist_ok=False)
    sys.path.insert(0,str(args.reference/'standalone'))
    from image import read_png
    names={'coverage.json','coverage.png','after_draw.png','overlay.png'}
    checks=[]
    with zipfile.ZipFile(args.archive) as archive:
        assert set(archive.namelist())==names and len(archive.infolist())==4
        for name in sorted(names):
            raw=archive.read(name)
            if name.endswith('.json'):
                actual=json.loads(raw)
                expected=json.loads((args.expected/name).read_text('utf-8'))
                assert actual==expected,name
                checks.append(dict(name=name,passed=True,event=actual['event']['id']))
            else:
                path=args.out/name
                path.write_bytes(raw)
                actual=read_png(path)
                expected=read_png(args.expected/name)
                assert actual==expected,name
                checks.append(dict(name=name,passed=True,width=actual[0],height=actual[1],rgba_sha256=hashlib.sha256(actual[2]).hexdigest()))
    (args.out/'validation.json').write_text(json.dumps(dict(passed=True,checks=checks),indent=2))
    print(json.dumps(dict(passed=True,checks=len(checks))))


if __name__=='__main__':
    main()
