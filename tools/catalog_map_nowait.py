"""Register original writable NOWAIT capture boundaries from frozen producer metadata."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--captures',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();root=args.captures.resolve(strict=True)
    source=root/'manifest.json';original=json.loads(source.read_text())
    assert original['completed'] and len(original['cases'])==8
    result=dict(schema='FloraGPA compatibility corpus 1',profile='GPA 2025 R1 legacy DX11 / IGPA v3',
                source_manifest_sha256=sha(source),cases=[])
    for row in original['cases']:
        assert sha(root/row['path'])==row['sha256']
        success=[w for w in row['writes'] if w['result']==0];assert len(success)==1
        write=success[0];failed=[w for w in row['writes'] if w['result']==-2005270518 and w['event']<write['event']]
        assert failed and all(w['resource']==write['resource'] and w['data_id']==0 for w in failed)
        if write['data_type']==1:
            assert write['storage_sha256']==row['after_sha256']
        else:
            raw=(root/row['path']).read_bytes();table=struct.unpack_from('<Q',raw,0xf4)[0]
            entries=[struct.unpack_from('<QQIBBH',raw,table+n*24) for n in range(struct.unpack_from('<I',raw,12)[0])]
            data=[e for e in entries if e[0]==write['data_id']];assert len(data)==1
            _,offset,size,_,category,kind=data[0];assert category==9 and kind==0x100 and row['mode']%4==0
            payload=raw[offset:offset+size];headers,length=struct.unpack_from('<II',payload)
            assert headers%8==0 and len(payload)==8+headers+length
            restored=bytearray((root/str(row['mode'])/'hardware/before.bin').read_bytes())
            assert hashlib.sha256(restored).hexdigest()==row['before_sha256']
            cursor=8+headers
            for n in range(0,headers,8):
                start,length=struct.unpack_from('<II',payload,8+n)
                assert start<=len(restored) and length<=len(restored)-start and cursor+length<=len(payload)
                restored[start:start+length]=payload[cursor:cursor+length];cursor+=length
            assert cursor==len(payload) and hashlib.sha256(restored).hexdigest()==row['after_sha256']
        boundary=dict(kind='buffer' if row['mode']%4==0 else 'texture',resource=write['resource'],expect_stable=True)
        result['cases'].append(dict(id=f'map_nowait_{row["mode"]}',path=row['path'],sha256=row['sha256'],bytes=row['bytes'],
            family='map_nowait',origin='self_owned_original_gpa_capture',
            source_manifests=[dict(path='manifest.json',sha256=sha(source),contains_capture_hash=True)],
            provenance_note='Unmodified original capture; full native hardware/WARP and injected buffer/texture byte oracles.',
            device_scope='Default hardware capture; native hardware/WARP verified; original player device not matched.',
            comparison_policy='exact_golden',reference_rgba_sha256=hashlib.sha256(bytes([0,255,0,255])*64).hexdigest(),
            reference_scope='Green present marker only; Map fidelity requires before/after complete resource byte boundaries.',
            boundaries=[boundary|dict(event=failed[0]['event'],expected_storage_sha256=row['before_sha256']),
                        boundary|dict(event=write['event'],expected_storage_sha256=row['after_sha256'])],
            controls=[],covers=[f'dimension {row["mode"]%4}',f'Map kind {write["kind"]}',
                               'saved failed polling attempts','successful writable DO_NOT_WAIT','padded native storage'],
            native_workload_fidelity='passed',
            fidelity_reason='Saved full/differential write bytes and copy-source bytes match native hardware/WARP/injected CPU oracles; marker alone is insufficient.'))
    args.out.parent.mkdir(parents=True,exist_ok=True)
    with args.out.open('x',encoding='utf-8') as output:json.dump(result,output,indent=2);output.write('\n')


if __name__=='__main__':main()
