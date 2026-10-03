"""CPU-only development harness regression checks; no GPA binaries needed."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from validate_corpus import image_difference, original_rgba, summarize_native, run, prioritize, original_kernel_completed
from catalog_captures import catalog, digest
from validate_corpus import validation_failed, original_comparison_status, record_original_comparison

class CompatibilityToolsTests(unittest.TestCase):
    def test_later_original_difference_is_not_hidden_by_first_equal_pair(self):
        item = {'id':'dynamic', 'status':'repeat_stable', 'original_status':'repeat_stable',
                'native':[{},{}], 'original':[{},{}],
                'native_original_comparisons':[{'byte_equal':True},{'byte_equal':True}]}
        self.assertEqual(original_comparison_status(item),'observed_equal')
        item['native_original_comparisons'][1]['byte_equal'] = False
        self.assertEqual(original_comparison_status(item),'observed_difference')
        queue = {}
        record_original_comparison(item,queue)
        record_original_comparison(item,queue)
        finding = next(iter(queue.values()))
        self.assertEqual(finding['captures'],['dynamic'])
        self.assertEqual(finding['occurrences'],1)
        self.assertEqual(finding['severity'],'warning')
        record_original_comparison(dict(item,id='second'),queue)
        self.assertEqual(finding['captures'],['dynamic','second'])
        self.assertEqual(finding['occurrences'],2)

    def test_completed_player_without_comparable_output_is_not_equal(self):
        good = {'status':'repeat_stable','original_status':'repeat_stable',
                'native':[{},{}],'original':[{},{}]}
        self.assertEqual(original_comparison_status(good),'unavailable')
        for pairs in [[],[{'byte_equal':True}],[{'byte_equal':True},{}],
                      [{'byte_equal':True},{'byte_equal':1}]]:
            self.assertEqual(original_comparison_status(dict(good,native_original_comparisons=pairs)),
                             'unavailable')
        self.assertEqual(original_comparison_status(dict(good,original_status='not_run')),'not_run')
        self.assertEqual(original_comparison_status(dict(good,status='preflight_only')),'not_run')
        good['native_original_comparisons']=[{'byte_equal':True}]*2
        for key, value in [('original_status','export_adapter_failed'),('status','replay_failed'),
                           ('status','golden_mismatch'),('status','replayed_without_image')]:
            self.assertEqual(original_comparison_status(dict(good,**{key:value})),'unavailable')

    def test_difference_is_review_evidence_without_relaxing_golden(self):
        item = {'id':'different','status':'repeat_stable','original_status':'repeat_stable',
                'preflight':{'exit_code':0,'report':{'status':'checked','errors':0}},
                'native':[{},{}],'original':[{},{}],
                'native_original_comparisons':[{'byte_equal':False}]*2}
        queue = {};record_original_comparison(item,queue)
        self.assertFalse(validation_failed({'cases':[item]}))
        self.assertEqual(item['original_comparison_status'],'observed_difference')
        self.assertTrue(queue)
        item['status']='golden_mismatch'
        self.assertTrue(validation_failed({'cases':[item]}))

    def test_batch_exit_rejects_recorded_failures(self):
        good = {'status':'repeat_stable', 'preflight':{'exit_code':0,
                'report':{'status':'review_required','errors':0}}, 'original_status':'not_run'}
        self.assertFalse(validation_failed({'cases':[good]}))
        for status in ['preflight_only','repeat_variable','replayed_without_image']:
            self.assertFalse(validation_failed({'cases':[dict(good,status=status)]}))
        for status in ['capture_missing_or_hash_mismatch','golden_mismatch','replay_failed']:
            self.assertTrue(validation_failed({'cases':[dict(good,status=status)]}))
        for preflight in [{}, {'exit_code':1,'report':{}},
                {'exit_code':0,'report':{'status':'blocked','errors':1}},
                {'exit_code':0,'report':{'status':'checked','errors':1}}]:
            self.assertTrue(validation_failed({'cases':[dict(good,preflight=preflight)]}))
        for status in ['replay_or_export_failed','export_adapter_failed']:
            self.assertTrue(validation_failed({'cases':[dict(good,original_status=status)]}))
        for key, value in [('controls',[{'passed':False}]),('boundaries',[{'passed':False}]),
                           ('native',[{'runtime_dependency_audit':'failed'}])]:
            self.assertTrue(validation_failed({'cases':[dict(good,**{key:value})]}))
        self.assertTrue(validation_failed({'cases':[]}))
        self.assertEqual(summarize_native([],{}),'replay_failed')
    def test_kernel_success_requires_complete_evidence(self):
        success = {'open_status':0, 'playback_status':0, 'closed':True, 'callbacks':[]}
        self.assertTrue(original_kernel_completed(success))
        for key in success:
            partial = dict(success); del partial[key]
            self.assertFalse(original_kernel_completed(partial))
        for key, value in [('open_status',13), ('playback_status',1), ('closed',False), ('callbacks',[{}])]:
            failed = dict(success); failed[key] = value
            self.assertFalse(original_kernel_completed(failed))

    def test_golden_failure_is_not_tolerated(self):
        runs=[{'exit_code':0,'report':{'completed':True,'rgba_sha256':'wrong'}}]*2
        self.assertEqual(summarize_native(runs,{'reference_rgba_sha256':'right'}),'golden_mismatch')

    def test_no_image_and_failure_are_distinct(self):
        self.assertEqual(summarize_native([{'exit_code':0,'report':{'completed':True}}]*2,{}),'replayed_without_image')
        self.assertEqual(summarize_native([{'exit_code':2,'report':{}}]*2,{}),'replay_failed')

    def test_variance_remains_visible(self):
        runs=[{'exit_code':0,'report':{'completed':True,'rgba_sha256':h}} for h in ['a','b']]
        self.assertEqual(summarize_native(runs,{}),'repeat_variable')
        d=image_difference(bytes([0,1,2,255]),bytes([2,1,4,255]))
        self.assertEqual(d['changed_bytes'],2)
        self.assertEqual(d['max_absolute_channel'],2)
        self.assertFalse(d['byte_equal'])

    def test_original_tga_orientation_and_channels(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);head=bytearray(18);head[2]=2
            struct.pack_into('<HHBB',head,12,1,2,32,0)
            (p/'framebuffer_new.tga').write_bytes(head+bytes([3,2,1,255,6,5,4,128]))
            self.assertEqual(original_rgba(p),bytes([4,5,6,128,1,2,3,255]))

    def test_truncated_tga_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'framebuffer_new.tga').write_bytes(b'bad')
            with self.assertRaisesRegex(ValueError,'Truncated'):original_rgba(p)

    def test_failed_child_launch_retains_evidence(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)
            r=run([p/'missing.exe'],p/'child.log',1,{})
            self.assertIsNone(r['exit_code'])
            self.assertIn('launch_error',r)
            self.assertGreater((p/'child.log').stat().st_size,0)

    def test_queue_priorities(self):
        cases=[{'id':'game','origin':'user_supplied_game_capture'}, {'id':'fixture','origin':'local_research_fixture'}]
        f={'captures':['game'],'severity':'error','kind':'replay_failed'}
        self.assertEqual(prioritize(f,cases)['priority'],0)
        f.update(captures=['fixture'],python_evidence='recovered implementation')
        self.assertEqual(prioritize(f,cases)['priority'],1)
        del f['python_evidence'];f['kind']='auxiliary_audit'
        self.assertEqual(prioritize(f,cases)['priority'],2)
        f['kind']='implementation_gap'
        self.assertEqual(prioritize(f,cases)['priority'],3)

    def test_catalog_hash_provenance_does_not_certify_modified_capture(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);folder=root/'analysis/capture_samples/legacy';folder.mkdir(parents=True)
            cap=folder/'modified.gpa_frame';cap.write_bytes(b'fixture')
            (folder/'manifest.json').write_text(json.dumps({'original_gpa_capture':True,'sha256':digest(cap)}))
            c=catalog(root)['cases'][0]
            self.assertEqual(c['origin'],'local_research_fixture')
            self.assertTrue(c['source_manifests'][0]['contains_capture_hash'])
            self.assertIsNone(c['reference_rgba_sha256'])

if __name__=='__main__':unittest.main()
