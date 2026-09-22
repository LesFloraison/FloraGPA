"""Private Windows priority-table interoperability with Python and optional GPA peers."""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import uuid


class Worker:
    def __init__(self,command,env):
        self.process=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env)
        self.lines=queue.Queue()
        def reader():
            for line in self.process.stdout:self.lines.put(line)
            self.lines.put(None)
        threading.Thread(target=reader,daemon=True).start();self.ready=self.read()
        if not self.ready.get('ready'):raise RuntimeError(self.ready)
    def read(self):
        line=self.lines.get(timeout=20)
        if line is None:raise RuntimeError(self.process.stderr.read())
        return json.loads(line)
    def call(self,operation,**kwargs):
        self.process.stdin.write(json.dumps(dict(operation=operation,**kwargs))+'\n');self.process.stdin.flush()
        value=self.read()
        if 'error' in value:raise RemoteError(value['error'])
        return value['result']
    def kill(self):
        if self.process.poll() is None:self.process.kill();self.process.wait(timeout=10)
    def close(self):
        if self.process.poll() is None:self.call('close');self.process.wait(timeout=10)
        if self.process.returncode:raise RuntimeError(self.process.stderr.read())


class RemoteError(RuntimeError):
    def __init__(self,data):super().__init__(str(data));self.data=data


def modules(pid):
    kernel=c.WinDLL('kernel32',use_last_error=True);psapi=c.WinDLL('psapi',use_last_error=True)
    kernel.OpenProcess.argtypes=[c.c_uint32,c.c_int,c.c_uint32];kernel.OpenProcess.restype=c.c_void_p
    kernel.CloseHandle.argtypes=[c.c_void_p]
    psapi.EnumProcessModulesEx.argtypes=[c.c_void_p,c.POINTER(c.c_void_p),c.c_uint32,c.POINTER(c.c_uint32),c.c_uint32]
    psapi.GetModuleFileNameExW.argtypes=[c.c_void_p,c.c_void_p,c.c_wchar_p,c.c_uint32]
    handle=kernel.OpenProcess(0x410,False,pid)
    if not handle:raise c.WinError(c.get_last_error())
    try:
        slots=(c.c_void_p*4096)();size=c.c_uint32()
        if not psapi.EnumProcessModulesEx(handle,slots,c.sizeof(slots),c.byref(size),3):raise c.WinError(c.get_last_error())
        assert size.value<=c.sizeof(slots)
        out=[]
        for i in range(size.value//c.sizeof(c.c_void_p)):
            text=c.create_unicode_buffer(32768)
            if not psapi.GetModuleFileNameExW(handle,slots[i],text,len(text)):raise c.WinError(c.get_last_error())
            out.append(text.value)
        return out
    finally:kernel.CloseHandle(handle)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['reference','exe','qt-bin','out']:p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--gpa-peer',action='store_true');a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
    windows=Path(os.environ['SystemRoot']);env={k:v for k,v in os.environ.items() if k.upper() in {'SYSTEMROOT','SYSTEMDRIVE','WINDIR','COMSPEC','TEMP','TMP','APPDATA','LOCALAPPDATA'}}
    env['PATH']=str(a.qt_bin.resolve())+os.pathsep+str(windows/'System32')+os.pathsep+str(windows)
    workers=[];checks=[];audits=[];runtime=[];peers=[]
    def check(label,value,expected=True):
        checks.append(dict(name=label,passed=value==expected));assert value==expected,(label,value,expected)
    def context(label):return a.out/(label+'.table'),'FloraGPA_NativePriority_'+uuid.uuid4().hex
    def native(path,name,resource='oa'):
        w=Worker([str(a.exe.resolve()),'--worker',str(path.resolve()),name,resource],env);workers.append(w)
        loaded=modules(w.process.pid);runtime.append(dict(pid=w.process.pid,modules=loaded))
        check('native process excludes Python and GPA',not any('intelswtools/gpa/' in x.replace('\\','/').lower() or Path(x).name.lower().startswith(('python','_tkinter')) for x in loaded))
        return w
    def peer(path,name,gpa=False,resource='oa'):
        command=[sys.executable,'-I',str(a.reference/'tools/priority_mutex_worker.py'),'--path',str(path.resolve()),'--mutex',name,'--resource',resource]
        if gpa:command.append('--native')
        w=Worker(command,os.environ.copy());workers.append(w);peers.append(dict(kind='gpa' if gpa else 'python',ready=w.ready));return w
    def timeout(label,action):
        try:action()
        except RemoteError as e:check(label,e.data['type'],'TimeoutError')
        else:raise AssertionError(label)
    try:
        for kind in ['cpp','python']+(['gpa'] if a.gpa_peer else []):
            path,name=context(kind);other=native(path,name) if kind=='cpp' else peer(path,name,kind=='gpa');own=native(path,name)
            other.call('priority',value=7);own.call('priority',value=7)
            check(kind+' peer enters',other.call('acquire'));check(kind+' exclusion',own.call('acquire'),False)
            own.call('priority',value=8);check(kind+' no preemption',own.call('acquire'),False)
            other.call('release');check(kind+' high priority enters',own.call('acquire'))
            check(kind+' peer sees owner',other.call('acquire'),False);check(kind+' repeat ownership',own.call('acquire'))
            own.call('release');check(kind+' one release clears depth',own.call('audit')['depth'],0)
            check(kind+' pending priority excludes peer',other.call('acquire'),False)
            own.call('priority',value=0xffffffff);check(kind+' withdrawal admits peer',other.call('acquire'))
            own.call('priority',value=7);timeout(kind+' bounded resource timeout',lambda:own.call('wait'))
            audit=own.call('audit');check(kind+' timeout withdraws',audit['priority'],0xffffffff);check(kind+' timeout recorded',audit['timeouts'],1);check(kind+' timeout unowned',audit['depth'],0)
            other.call('release');own.call('priority',value=7)
            other.call('hold_guard');before=path.read_bytes();timeout(kind+' guard timeout',lambda:own.call('acquire'));check(kind+' guard preserves bytes',path.read_bytes(),before)
            other.call('release_guard');check(kind+' retry after guard',own.call('acquire'))
            own.call('release');other.close();audits.append(own.call('audit'));own.close();check(kind+' empty storage removed',path.exists(),False)

        path,name=context('global');ui=native(path,name,'UI');global_lock=native(path,name,'GLOBAL');concrete=native(path,name)
        ui.call('priority',value=8);check('UI owns',ui.call('acquire'));global_lock.call('priority',value=9)
        check('GLOBAL ignores UI',global_lock.call('acquire'));concrete.call('priority',value=7)
        check('GLOBAL excludes concrete',concrete.call('acquire'),False);global_lock.call('release')
        check('pending GLOBAL does not block concrete grant',concrete.call('acquire'));check('pending GLOBAL excludes eligibility',concrete.call('eligible'),False)
        check('highest is GLOBAL',concrete.call('highest'),9);check('concrete excludes GLOBAL',global_lock.call('acquire'),False)
        concrete.close();global_lock.close();ui.close();check('GLOBAL storage removed',path.exists(),False)

        for priority in [7,8]:
            path,name=context('crash'+str(priority));other=native(path,name);other.call('priority',value=priority);check('crash owner enters',other.call('acquire'))
            own=native(path,name);own.call('priority',value=7);other.kill()
            check('dead owner cleanup',own.call('acquire'),priority==7)
            if priority==8:
                check('stale maximum retained',own.call('highest'),8);own.call('priority',value=7);check('priority update recovers',own.call('acquire'))
            own.close();check('dead table removed',path.exists(),False)
        path,name=context('abandoned');other=native(path,name);own=native(path,name);other.call('hold_guard');other.kill()
        own.call('priority',value=7);check('abandoned guard counted',own.call('audit')['abandoned_guards'],1)
        check('abandoned guard recovery',own.call('acquire'));own.close();check('abandoned storage removed',path.exists(),False)
        report=dict(passed=True,checks=checks,audits=audits,native_processes=runtime,peers=peers,
            reference_worker_sha256=hashlib.sha256((a.reference/'tools/priority_mutex_worker.py').read_bytes()).hexdigest(),shared_gpa_file_touched=False,gpu_execution=False,
            original_gpa_peer=a.gpa_peer,exe_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest())
        (a.out/'validation.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
        print(json.dumps(dict(passed=True,checks=len(checks),native_processes=len(runtime),original_gpa_peer=a.gpa_peer)))
    finally:
        for w in workers:w.kill()


if __name__=='__main__':main()
