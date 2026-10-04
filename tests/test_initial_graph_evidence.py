"""Counterexamples for full initial dependency metadata acceptance."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import test_initial_cache_evidence as cache_evidence
from validate_initial_graph_evidence import check_graph


class GraphEvidenceTests(unittest.TestCase):
    def fixture(self):
        trace, commands, entries = cache_evidence.CacheEvidenceTests().fixture()
        trace['node_reference_collectors'] = [dict(id=20, kind='resource', wire_type=0x83,
                                                  collector_rva='0x5b960', references=[])]
        trace['node_factories'] = [dict(id=20, kind='resource', wire_type=0x83, present=True)]
        erg = dict(commands['commands'][0]['original_initialization'], id=10, kind='erg', type=0x3e)
        resource = dict(id=20, kind='resource', type=0x83, status='recovered', collector_rva='0x5b960',
                        collector_sequence=[], dependency_set=[], resource_objects_validated=False,
                        scope='original_initial_dependency_collection')
        export = dict(cache=copy.deepcopy(commands['original_initialization_cache']), nodes=[erg, resource],
                      dependency_graph_complete=True, issues=[], initialization_schedule_available=False,
                      execution_supported=False)
        return trace, commands, entries, export

    def test_complete_initial_metadata(self):
        self.assertEqual(check_graph(*self.fixture())['exact_nodes'], 2)

    def test_missing_duplicate_or_extra_nodes(self):
        for field in ['node_reference_collectors','node_factories','nodes']:
            for mode in ['missing','duplicate']:
                t,c,e,g = self.fixture(); rows = g[field] if field == 'nodes' else t[field]
                if mode == 'missing': rows.pop()
                else: rows.append(copy.deepcopy(rows[-1]))
                with self.subTest(field=field, mode=mode), self.assertRaises(ValueError): check_graph(t,c,e,g)

    def test_different_factory_type_or_missing_object(self):
        for field,value in [('present',False),('wire_type',0x84),('kind','data')]:
            t,c,e,g = self.fixture(); t['node_factories'][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): check_graph(t,c,e,g)

    def test_node_order_dependencies_and_scope(self):
        for field,value in [('collector_sequence',[20]),('dependency_set',[20]),('status','unrecovered'),
                            ('collector_rva','0x5c210'),('type',0x85),('kind','state'),
                            ('resource_objects_validated',True),('scope','execution')]:
            t,c,e,g = self.fixture(); g['nodes'][1][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): check_graph(t,c,e,g)

    def test_metadata_is_not_execution_acceptance(self):
        for field,value in [('execution_supported',True),('initialization_schedule_available',True),
                            ('dependency_graph_complete',False),('issues',[{'id':20}])]:
            t,c,e,g = self.fixture(); g[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): check_graph(t,c,e,g)

    def test_noninitial_resource_version(self):
        t,c,e,g = self.fixture(); t['graph_before_first_erg_initialize'][1]['version'] = 1
        with self.assertRaises(ValueError): check_graph(t,c,e,g)


if __name__ == '__main__': unittest.main()
