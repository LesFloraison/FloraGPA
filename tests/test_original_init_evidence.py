"""CPU counterexamples for original initialization trace claims."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from validate_original_init_traces import check_trace
from probe_original_init_scheduler import predicted


class InitEvidenceTests(unittest.TestCase):
    def fixture(self):
        return dict(completed=True,patches_restored=True,observer_errors=[],observers_enabled=True,
                    replay=dict(closed=True,open_status=0,playback_status=0,callbacks=[]),
                    observations=[dict(kind=k,event=10,thread=1) for k in
                                  ['create','initialize_enter','initialize_return']],
                    graph_before_first_erg_initialize=[dict(id=10,kind='erg',version=0,status=1,dependencies=[20]),
                                                       dict(id=20,kind='resource',version=0,status=2,dependencies=[])],
                    initializer_stacks=[dict(event=10,player_return_rvas=['0x9a703','0x9a815','0x9ad86','0x92a7c'])])

    def test_completed_zero_version_trace(self):
        self.assertEqual(check_trace(self.fixture()),1)

    def test_unrestored_hook_or_original_error_is_not_success(self):
        for field in ['patches_restored','completed','original_callback','observer_error']:
            trace=self.fixture()
            if field in trace:
                trace[field]=False
            elif field=='original_callback':
                trace['replay']['callbacks']=[{'error':'native error'}]
            else:
                trace['observer_errors']=['layout mismatch']
            with self.subTest(field=field),self.assertRaises(ValueError):
                check_trace(trace)

    def test_reordered_missing_and_duplicate_callbacks_reject(self):
        original=self.fixture()
        for observations in [original['observations'][:-1],original['observations'][::-1],
                             original['observations']+original['observations'][1:]]:
            trace=copy.deepcopy(original)
            trace['observations']=observations
            with self.assertRaises(ValueError):
                check_trace(trace)

    def test_unready_or_erg_dependency_cannot_claim_sorted_scope(self):
        for field,value in [('status',1),('kind','erg')]:
            trace=self.fixture()
            trace['graph_before_first_erg_initialize'][1][field]=value
            with self.assertRaises(ValueError):
                check_trace(trace)

    def test_missing_stack_is_not_proof(self):
        trace=self.fixture()
        trace['initializer_stacks']=[]
        with self.assertRaises(ValueError):
            check_trace(trace)

    def test_disabled_observer_control_must_be_uninstrumented(self):
        trace=self.fixture()
        trace.update(observers_enabled=False,observations=[],patched_slots=0)
        self.assertEqual(check_trace(trace),0)
        trace['patched_slots']=1
        with self.assertRaises(ValueError):
            check_trace(trace)

    def test_dependency_schedule_is_not_id_sort(self):
        graph=[dict(id=10,kind=3,dependencies=[20]),dict(id=20,kind=3,dependencies=[])]
        self.assertEqual(predicted(graph,set())[0],[20,10])
        self.assertEqual(predicted(graph,{20})[0],[10,20,10])


if __name__=='__main__':
    unittest.main()
