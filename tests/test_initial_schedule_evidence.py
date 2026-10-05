"""False-success counterexamples for original/C++ initialization scheduling."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from validate_initial_schedule_evidence import check_schedule


class ScheduleEvidenceTests(unittest.TestCase):
    def fixture(self):
        initial = [dict(id=1,kind='resource',version=0,status=1,dependents=[],dependencies=[]),
                   dict(id=2,kind='erg',version=0,status=1,dependents=[],dependencies=[1])]
        calls = [dict(id=n['id'], kind=n['kind'], version=0, thread=1, before_status=1, after_status=2,
                      dependencies=n['dependencies'], initializer_rva='0x9a6d0' if n['id']==1 else '0x9a6f0')
                 for n in initial]
        trace = dict(graph_before_first_node_initialize=initial, node_initializations=calls,
                     observations=[dict(kind='initialize_enter',event=2)])
        graph = dict(dependency_graph_complete=True, issues=[], execution_supported=False,
                     initialization_schedule_available=False,
                     nodes=[dict(id=n['id'],kind=n['kind'],status='recovered',dependency_set=n['dependencies']) for n in initial],
                     modeled_initialization=dict(status='modeled',scope='initial_version_fresh_dependents',
                         assumes_all_initializers_succeed=True,gpu_execution_verified=False,
                         order=[1,2],previous_statuses=[1,1]))
        return trace,graph

    def test_matches_complete_sequence(self):
        self.assertEqual(check_schedule(*self.fixture())['calls'],2)

    def test_missing_or_duplicate_node(self):
        for target in ['native','cpp']:
            for duplicate in [False,True]:
                t,g=self.fixture(); rows=t['graph_before_first_node_initialize'] if target=='native' else g['nodes']
                if duplicate:rows.append(copy.deepcopy(rows[0]))
                else:rows.pop()
                with self.assertRaises(ValueError):check_schedule(t,g)

    def test_wrong_initial_state_or_version(self):
        for field,value in [('status',2),('version',1),('dependents',[2])]:
            t,g=self.fixture();t['graph_before_first_node_initialize'][0][field]=value
            with self.assertRaises(ValueError):check_schedule(t,g)

    def test_call_order_and_completion(self):
        for field,value in [('before_status',2),('after_status',1),('version',1),('thread',2),
                            ('initializer_rva','0x9a710'),('dependencies',[]),('kind','state')]:
            t,g=self.fixture();t['node_initializations'][1][field]=value
            with self.assertRaises(ValueError):check_schedule(t,g)
        t,g=self.fixture();t['node_initializations'].reverse()
        with self.assertRaises(ValueError):check_schedule(t,g)

    def test_model_scope_cannot_be_execution(self):
        for field,value in [('status','unavailable'),('scope','edited_version'),
                            ('assumes_all_initializers_succeed',False),('gpu_execution_verified',True),
                            ('order',[2,1]),('previous_statuses',[1,2])]:
            t,g=self.fixture();g['modeled_initialization'][field]=value
            with self.assertRaises(ValueError):check_schedule(t,g)
        for field,value in [('execution_supported',True),('initialization_schedule_available',True),
                            ('dependency_graph_complete',False),('issues',[{'id':1}])]:
            t,g=self.fixture();g[field]=value
            with self.assertRaises(ValueError):check_schedule(t,g)

    def test_missing_native_call_even_if_model_matches(self):
        t,g=self.fixture();t['node_initializations'].pop();g['modeled_initialization'].update(order=[1],previous_statuses=[1])
        with self.assertRaises(ValueError):check_schedule(t,g)

    def test_missing_prerequisite_and_unready_order(self):
        t,g=self.fixture();g['nodes'][1]['dependency_set']=[9]
        with self.assertRaises(ValueError):check_schedule(t,g)
        t,g=self.fixture();t['node_initializations'].reverse();g['modeled_initialization']['order']=[2,1]
        with self.assertRaises(ValueError):check_schedule(t,g)

    def test_erg_observer_must_agree(self):
        t,g=self.fixture();t['observations']=[]
        with self.assertRaises(ValueError):check_schedule(t,g)


if __name__=='__main__':unittest.main()
