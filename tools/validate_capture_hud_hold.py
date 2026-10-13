"""Check a user-reported physical capture hold against runtime counter snapshots."""
import argparse
import hashlib
import json
from pathlib import Path


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('before','after','session','out'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--user-reported-seconds',type=float,required=True,
                        help='User attestation; not an automatically measured duration')
    args=parser.parse_args()
    if args.user_reported_seconds<=0:parser.error('A positive user-reported duration is required')
    def read(path):return json.loads(path.read_text(encoding='utf-8-sig'))
    before,after=read(args.before),read(args.after)
    checks=[]
    result={'passed':False,'reported_hold_seconds':args.user_reported_seconds,
            'duration_source':'User report, not runtime measurement','checks':checks}
    def check(name,passed):
        checks.append({'name':name,'passed':bool(passed)})
        args.out.write_text(json.dumps(result,indent=2)+'\n')
        if not passed:raise AssertionError(name)
    check('same-session',before['pid']==after['pid'] and before['session_id']==after['session_id'])
    check('connected',before['hud']['connected'] and after['hud']['connected'])
    old=before['hud']['input_diagnostics'];new=after['hud']['input_diagnostics']
    result['counter_deltas']={key:new[key]-old[key] for key in old}
    for key in ('capture_edges','hotkey_request_attempts','hotkey_requests_accepted'):
        check('exactly-one-'+key,result['counter_deltas'][key]==1)
    result['single_submission_measured']=True
    result['legacy_repeat_suppression_verified']=result['counter_deltas']['legacy_capture_repeat_messages']>0
    check('no-mode-change',result['counter_deltas']['mode_edges']==0 and before['hud']['mode']==after['hud']['mode'])
    progress=after['capture_progress'];request=after['hud']['last_request']
    check('settled-request',not progress['busy'] and not after['hud']['request_pending'] and progress['state'] in ('Completed','Failed'))
    previous_request=before['hud']['last_request'] or {}
    check('request-identity',request['ok'] and request['request_name']==progress['request_name'] and request['request_name']!=previous_request.get('request_name'))
    directory=args.session.resolve();frame=directory/progress['request_name']
    check('bounded-output-name',frame.parent==directory and frame.suffix=='.gpa_frame')
    if progress['state']=='Completed':
        check('published-file',Path(progress['file']).resolve()==frame and frame.is_file())
        result['capture_sha256']=hashlib.sha256(frame.read_bytes()).hexdigest()
        result['capture_file']=str(frame)
        result['replay_verified']=False  # A separate GPU oracle is required.
    else:
        check('explicit-failure-without-publication',bool(progress.get('error')) and not frame.exists())
        result['capture_error']=progress['error']
    check('no-partial-file',not list(directory.glob('*.partial')))
    result['snapshot_sha256']={str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in (args.before,args.after)}
    # Preserve useful submission/file evidence even when Windows delivered no
    # typematic messages. The strict repeat-suppression gate remains unchanged.
    check('legacy-repeats-observed',result['legacy_repeat_suppression_verified'])
    result['passed']=True;args.out.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))


if __name__=='__main__':main()
