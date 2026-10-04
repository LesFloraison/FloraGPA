"""Counterexamples for file-to-original-initializer comparison claims."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import test_original_init_evidence as init_evidence
from validate_original_reference_evidence import check_references


class ReferenceEvidenceTests(unittest.TestCase):
    def fixture(self):
        trace = init_evidence.InitEvidenceTests().fixture()
        trace['reference_collectors'] = [dict(event=10,wire_type=0x3e,
            collector_rva='0x23e20',references=[20,20])]
        commands = dict(commands=[dict(id=10,type=0x3e,status='decoded',original_initialization=dict(
            status='recovered',collector_rva='0x23e20',collector_sequence=[20,20],dependency_set=[20],
            execution_dependencies_complete=False,registration_sequence_available=False))])
        return trace,commands

    def test_equal_graph_does_not_erase_collector_duplicates(self):
        trace,commands=self.fixture()
        self.assertEqual(check_references(trace,commands)['recovered'],1)
        commands['commands'][0]['original_initialization']['collector_sequence']=[20]
        with self.assertRaises(ValueError): check_references(trace,commands)

    def test_missing_duplicate_or_wrong_type_observation(self):
        for variant in ['missing','duplicate','type']:
            trace,commands=self.fixture()
            if variant=='missing': trace['reference_collectors']=[]
            if variant=='duplicate': trace['reference_collectors']*=2
            if variant=='type': trace['reference_collectors'][0]['wire_type']=0x40
            with self.subTest(variant=variant),self.assertRaises(ValueError): check_references(trace,commands)

    def test_different_native_graph_rejected(self):
        trace,commands=self.fixture()
        trace['reference_collectors'][0]['references']=[]
        with self.assertRaises(ValueError): check_references(trace,commands)

    def test_wrong_rva_partial_record_or_overstated_scope_rejected(self):
        for variant in ['rva','partial','scope','set']:
            trace,commands=self.fixture()
            row=commands['commands'][0]
            info=row['original_initialization']
            if variant=='rva': info['collector_rva']='0x23be0'
            if variant=='partial': row['status']='partial'
            if variant=='scope': info['execution_dependencies_complete']=True
            if variant=='set': info['dependency_set']=[]
            with self.subTest(variant=variant),self.assertRaises(ValueError): check_references(trace,commands)

    def test_conditional_gap_explicit_and_not_counted_as_recovered(self):
        trace,commands=self.fixture()
        trace['reference_collectors'][0]['wire_type']=0x25e
        row=commands['commands'][0]
        row['type']=0x25e
        row['original_initialization']=dict(status='unrecovered',reason='Native cache required',
            execution_dependencies_complete=False,registration_sequence_available=False)
        result=check_references(trace,commands)
        self.assertEqual(result['recovered'],0)
        self.assertEqual(result['unrecovered'],{'0x25e':1})
        row['original_initialization']['collector_sequence']=[]
        with self.assertRaises(ValueError): check_references(trace,commands)


if __name__=='__main__': unittest.main()
