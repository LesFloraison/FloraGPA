"""Manual fault isolation, not an original-capture repair or runtime dependency.

Usage: python talos-query-subset.py CAPTURE QUERY_COVERAGE_JSON NEW_OUTPUT_DIR
The coverage JSON is the query_replay_coverage object from command-lists.
Generated files stay outside Git. Unchanged files are hardlinked to the original;
use this directory only with the read-only replay tools, never edit those links.
See docs/DX12_REPLAY.md, "Talos continuation with a deliberately altered query subset".
"""
from pathlib import Path
import json,struct,os,hashlib,sys
if len(sys.argv)!=4:
    raise SystemExit('Expected CAPTURE QUERY_COVERAGE_JSON NEW_OUTPUT_DIR')
source=Path(sys.argv[1]).resolve(strict=True)
root=Path(sys.argv[3]).resolve()
if root==source or source in root.parents:
    raise ValueError('Output must be outside the original capture')
coverage=json.loads(Path(sys.argv[2]).read_text())['resolves_and_issues']
assert [(x['source_record'],x['missing_count']) for x in coverage]==[('188923',140),('192515',146)]
byrecord={int(x['source_record']):x for x in coverage}
thread=source/'thread-31428-13'; original=thread.read_bytes()
assert hashlib.sha256(original).hexdigest()=='db5bbf6e1ff0cd7483cce327a435c476ed96f18b1a595fab1f1519b180ce6db7', 'Unexpected Talos stream'
root.mkdir(exist_ok=False); target=root/'capture.gpa_frame';target.mkdir()
packets=[]; off=0; record=185466; changes=[]; additional=0
while off<len(original):
    ty,ts,flags,size=struct.unpack_from('<4Q',original,off);end=off+32+size; assert end<=len(original)
    record+=1
    if record not in byrecord: packets.append(original[off:end])
    else:
        row=byrecord[record]; assert ty==0x1ae3cdc411c2d and size==44
        cl,heap,qt,start,count,destination,destoff=struct.unpack_from('<QQIIIQQ',original,off+32)
        assert heap==int(row['query_heap']) and destination==int(row['destination']) and count==236 and qt==0 and destoff==0
        missing={i for r in row['missing_ranges'] for i in range(r['start'],r['start']+r['count'])}
        known=sorted(set(range(start,start+count))-missing); assert len(known)==row['completed_count']
        runs=[]
        for i in known:
            if runs and i==runs[-1][0]+runs[-1][1]: runs[-1][1]+=1
            else:runs.append([i,1])
        for begin,n in runs:
            payload=struct.pack('<QQIIIQQ',cl,heap,qt,begin,n,destination,destoff+(begin-start)*8)
            packets.append(struct.pack('<4Q',ty,ts,flags,len(payload))+payload)
        changes.append({'original_record':str(record),'original_execution':row['execution'],'original_file_offset':str(off),
          'destination':str(destination),'preserved_runs':runs,'omitted_ranges':row['missing_ranges'],'missing_count':row['missing_count'],
          'changed_execution_first':str(int(row['execution'])+additional)})
        additional+=len(runs)-1
    off=end
assert off==len(original) and record==192535 and len(changes)==2
for p in source.rglob('*'):
    assert not p.is_symlink()
    out=target/p.relative_to(source)
    if p.is_dir():out.mkdir(exist_ok=True)
    elif p!=thread:os.link(p,out)
modified=b''.join(packets);(target/thread.name).write_bytes(modified)
manifest={'scope':'Research-only modified capture. Missing query output writes are omitted, never reconstructed. Output is not original-frame fidelity evidence. All other original files are read-only hardlinks.',
          'original':str(source),'modified':str(target.resolve()),'changes':changes,'new_final_execution':str(19410+additional),
          'original_thread_sha256':hashlib.sha256(original).hexdigest(),'modified_thread_sha256':hashlib.sha256(modified).hexdigest(),
          'additional_packets':additional,'source_thread_unchanged':thread.read_bytes()==original}
(root/'mutation.json').write_text(json.dumps(manifest,indent=2));print(json.dumps(manifest,indent=2))
