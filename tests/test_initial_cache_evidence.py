"""Reject misleading initial-cache coverage and provenance claims."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import test_original_reference_evidence as reference_evidence
from validate_initial_cache_evidence import check_cache


class CacheEvidenceTests(unittest.TestCase):
    def fixture(self):
        trace,commands=reference_evidence.ReferenceEvidenceTests().fixture()
        cache=dict(categories=[dict(id=10,kind=3),dict(id=20,kind=2),dict(id=50,kind=3)],
                   descriptors=dict(state=[],resource=[20],erg=[10],data=[]))
        trace['cache_before_first_collector']=copy.deepcopy(cache)
        trace['cache_before_first_initialize']=copy.deepcopy(cache)
        trace['original_file_index']=[dict(id=i,kind=k,type=t) for i,k,t in [(10,3,0x3e),(20,2,0x83),(50,3,0x34ed)]]
        commands['original_initialization_cache']=dict(**cache,status='recovered',
            scope='unmodified_initial_file_registration',version=0,resource_objects_validated=False)
        entries={i:dict(category=k*2+1,type=t,flags=0) for i,k,t in [(10,3,0x3e),(20,2,0x83),(50,3,0x34ed)]}
        return trace,commands,entries

    def test_category_only_api_is_not_a_descriptor(self):
        result=check_cache(*self.fixture())
        self.assertEqual(result['category_only_ergs'],1)
        self.assertEqual(result['descriptor_entries'],2)

    def test_missing_duplicate_or_wrong_native_index(self):
        for mode in ['missing','duplicate','type','kind']:
            trace,commands,entries=self.fixture()
            index=trace['original_file_index']
            if mode=='missing': index.pop()
            if mode=='duplicate': index[2]=copy.deepcopy(index[0])
            if mode=='type': index[0]['type']=0x40
            if mode=='kind': index[0]['kind']=2
            with self.subTest(mode=mode),self.assertRaises(ValueError): check_cache(trace,commands,entries)

    def test_changing_cache_or_descriptor_graph_rejected(self):
        for mode in ['changing','graph','cpp']:
            trace,commands,entries=self.fixture()
            if mode=='changing': trace['cache_before_first_initialize']['descriptors']['erg'].append(50)
            if mode=='graph': trace['graph_before_first_erg_initialize'][1]['kind']='data'
            if mode=='cpp': commands['original_initialization_cache']['descriptors']['erg'].append(50)
            with self.subTest(mode=mode),self.assertRaises(ValueError): check_cache(trace,commands,entries)

    def test_unverified_index_profiles_reject(self):
        for key,value in [('flags',1),('category',2)]:
            trace,commands,entries=self.fixture();entries[20][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError): check_cache(trace,commands,entries)

    def test_membership_is_not_object_or_later_version_validation(self):
        for key,value in [('resource_objects_validated',True),('version',1),('status','partial')]:
            trace,commands,entries=self.fixture();commands['original_initialization_cache'][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError): check_cache(trace,commands,entries)


if __name__=='__main__': unittest.main()
