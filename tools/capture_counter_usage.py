"""Development-only original GPA captures of counter-independent indexed UAV stores."""
import argparse,hashlib,json,os,shutil,subprocess
from pathlib import Path

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(__doc__)
 p.add_argument('--producer',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
 p.add_argument('--mode',type=int,action='append',choices=[12,13,18,40,41,42],help='May be repeated; defaults to 12,13,18')
 p.add_argument('--gpa-dir',type=Path,default=Path('C:/Program Files/IntelSWTools/GPA'))
 a=p.parse_args(); root=a.out.resolve();root.mkdir(parents=True,exist_ok=False)
 repo=Path(__file__).resolve().parents[1];frozen=root/'producer';frozen.mkdir()
 for name in ['tools/capture_counter_usage.py','tools/native/buffer_view_creation_probe.cpp','tools/native/original_capture_control.h','tools/native/texture_probe_helpers.h','tools/native/present_capture_probe.cpp']:
  shutil.copy2(repo/name,frozen/Path(name).name)
 exe=a.producer.resolve(strict=True);shutil.copy2(exe,frozen/exe.name);exe=frozen/exe.name
 m={'schema':'FloraGPA compatibility corpus 1','profile':'GPA 2025 R1 legacy DX11 / IGPA v3','completed':False,'producer_files':{p.name:sha(p) for p in frozen.iterdir()},'gpa_sha256':{n:sha(a.gpa_dir/n) for n in ['shimloader64.dll','shimd3d64.dll','dx11_player.dll']},'cases':[],'producer_runs':[]}
 def save(): (root/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
 try:
  for mode in a.mode or [12,13,18]:
   folder=root/str(mode);folder.mkdir();capture=folder/'capture.gpa_frame'
   for backend in ['native','captured']:
    cmd=[str(exe),str(folder/backend),str(mode)];env=dict(os.environ);env.pop('GPA_LOCAL_INJECT',None)
    if backend=='captured': cmd.extend([str(capture),str(a.gpa_dir/'shimloader64.dll')]);env['GPA_LOCAL_INJECT']='true'
    run={'mode':mode,'backend':backend,'command':cmd};m['producer_runs'].append(run);save()
    with (folder/(backend+'.log')).open('wb') as log:
     result=subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW,timeout=60)
    run['exit_code']=result.returncode;save()
    if result.returncode: raise RuntimeError(f'Producer failed: {mode}/{backend}')
    oracle=json.loads((folder/backend/'oracle.json').read_text());run['oracle']=oracle
    assert oracle['completed'] and len(oracle['frames'])==12
    assert all(x['bytes_verified'] and x['image_verified'] for x in oracle['frames'])
    assert (folder/backend/'buffer.bin').read_bytes()==(folder/backend/'expected.bin').read_bytes()
    if mode >= 40:
     assert (folder/backend/'indexed-buffer.bin').read_bytes()==(folder/backend/'indexed-expected.bin').read_bytes()
    assert (folder/backend/'frame.rgba').read_bytes()==bytes([0,255,0,255])*64
    save()
   assert (folder/'native/buffer.bin').read_bytes()==(folder/'captured/buffer.bin').read_bytes()
   m['cases'].append({'id':f'counter_usage_{mode}','path':f'{mode}/capture.gpa_frame','sha256':sha(capture),'bytes':capture.stat().st_size,'family':'counter_usage','origin':'self_owned_original_gpa_capture','provenance_note':'Unmodified CaptureNextFrame output; native and injected full buffer/image oracles retained.','device_scope':'Producer adapter recorded; original replay adapter not yet identified.','source_manifests':[],'reference_rgba_sha256':sha(folder/'native/frame.rgba'),'reference_buffer_sha256':sha(folder/'native/buffer.bin'),'comparison_policy':'exact_golden','controls':[],'boundaries':[],'covers':['Counter UAV with indexed store and KEEP, no hidden counter read' if mode==18 else 'Initialized IncrementCounter control' if mode==13 else 'Ordinary structured UAV indexed store control']})
   if mode >= 40:
    assert (folder/'native/indexed-buffer.bin').read_bytes()==(folder/'captured/indexed-buffer.bin').read_bytes()
    m['cases'][-1]['reference_indexed_buffer_sha256']=sha(folder/'native/indexed-buffer.bin')
    m['cases'][-1]['covers']=['Counter operation on u3 with counter-independent u1 KEEP' if mode==42 else 'Known counters in both slots, indexed access to u1' if mode==41 else 'Counter operation on u0 with counter-independent u1 KEEP']
   save();print(mode,'original capture verified',flush=True)
  m['completed']=True
 except Exception as e: m['error']=str(e);raise
 finally: save()
if __name__=='__main__':main()
