"""Reject overclaims about native cloning, recollection and propagation."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from probe_original_reference_collectors import PLAYER
from validate_original_version_evidence import check_versions,check_primitives


class VersionEvidenceTests(unittest.TestCase):
    def fixture(self):
        kinds={10:'state',20:'resource',30:'data',40:'erg'}
        deps={10:[20],20:[30],30:[],40:[20]};children={10:[],20:[10,40],30:[20],40:[]}
        types={10:3,20:0x83,30:1,40:0x3e};category={'state':3,'resource':5,'data':9,'erg':7}
        rvas={'state':'0x9a710','resource':'0x9a6d0','data':'0x9a6d0','erg':'0x9a6f0'}
        initial=[dict(id=i,kind=kind,status=2,dependencies=deps[i],dependents=children[i],
                      object_identity=f'obj{i}',control_identity=f'ctrl{i}') for i,kind in kinds.items()]
        graph=dict(nodes=[dict(id=i,kind=kind,type=types[i],dependency_set=deps[i]) for i,kind in kinds.items()],
                   dependency_graph_complete=True,initialization_schedule_available=False,execution_supported=False)
        audit=dict(event_dispatch_count=1,event_function_rvas={'0x25f20':1},negative_control_suppress_draws=False)
        report=dict(completed=True,passed=True,closed=True,open_status=0,observers_restored=True,
                    observer_errors=[],callbacks=[],player_sha256=PLAYER,cloned_version_workload_playback=False,
                    cloned_version_resource_initialization=True,default_pixels_equal=True,
                    default_graph_unchanged_after_playback=True,default_execution_counts_equal=True,
                    default_playbacks=[dict(label=l,status=0,error=0,audit=copy.deepcopy(audit)) for l in ['before','after']],
                    initial_versions=[0],initial_graph=copy.deepcopy(initial),nodes=4,clones=[],saved_payload_reloads=[],
                    unregistered_hole_graph=[dict(id=i,kind=k,status=0,dependencies=[],dependents=[],
                                                 object_identity='0x0',control_identity='0x0') for i,k in kinds.items()])
        for index,(source,target) in enumerate([(0,7),(7,9)]):
            report['clones'].append(dict(source=source,target=target,equal=True,default_unchanged=True,
                registered_versions=[0,7,9][:index+2],source_graph=copy.deepcopy(initial),target_graph=copy.deepcopy(initial)))
        current=copy.deepcopy(initial)
        for i in [10,20,30,40]:
            before=copy.deepcopy(current);node=next(n for n in current if n['id']==i)
            node['object_identity']+='new';node['control_identity']+='new'
            report['saved_payload_reloads'].append(dict(id=i,category=category[kinds[i]],type=types[i],
                before=before,after=copy.deepcopy(current),default_unchanged=True,other_clone_unchanged=True,
                initializers=[dict(id=j,kind=kinds[j],version=7,initializer_rva=rvas[kinds[j]],
                                   before_status=1 if index==0 else 2,after_status=2)
                              for index,j in enumerate([i]+children[i])]))
        report['clones'].append(dict(source=7,target=11,equal=True,default_unchanged=True,registered_versions=[0,7,9,11],
                                     source_graph=copy.deepcopy(current),target_graph=copy.deepcopy(current)))
        return report,graph

    def test_all_kinds_clone_and_direct_propagation(self):
        self.assertEqual(check_versions(*self.fixture()),dict(nodes=4,clones=3,copied_nodes=12,reloads=4,initializer_calls=7))

    def test_missing_copy_or_false_registry(self):
        for mode in ['missing','target','registry','shared_object']:
            r,g=self.fixture()
            if mode=='missing':r['clones'].pop()
            if mode=='target':r['clones'][0]['target']=1
            if mode=='registry':r['clones'][0]['registered_versions'].append(1)
            if mode=='shared_object':r['clones'][0]['target_graph'][0]['object_identity']='unexpected'
            with self.subTest(mode=mode),self.assertRaises(ValueError):check_versions(r,g)

    def test_allocated_hole_is_not_registered_copy(self):
        for field,value in [('status',2),('dependencies',[20]),('object_identity','obj10')]:
            r,g=self.fixture();r['unregistered_hole_graph'][0][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):check_versions(r,g)

    def test_wrong_version_state_and_function(self):
        for field,value in [('version',0),('before_status',2),('after_status',1),('initializer_rva','0x9a6f0')]:
            r,g=self.fixture();r['saved_payload_reloads'][0]['initializers'][0][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):check_versions(r,g)

    def test_reinitialization_is_not_transitive_for_ready_children(self):
        for mode in ['extra_grandchild','missing_child','duplicate_child','sorted_instead_of_target_first']:
            r,g=self.fixture();calls=r['saved_payload_reloads'][1]['initializers']
            if mode=='extra_grandchild':r['saved_payload_reloads'][2]['initializers'].append(copy.deepcopy(calls[1]))
            if mode=='missing_child':calls.pop()
            if mode=='duplicate_child':calls.append(copy.deepcopy(calls[-1]))
            if mode=='sorted_instead_of_target_first':calls.sort(key=lambda c:c['id'])
            with self.subTest(mode=mode),self.assertRaises(ValueError):check_versions(r,g)

    def test_reload_cannot_reuse_old_wrapper_or_change_other_node(self):
        for mode in ['old_object','other_node','dependency','prior_clone']:
            r,g=self.fixture();row=r['saved_payload_reloads'][0]
            if mode=='old_object':row['after'][0]['object_identity']=row['before'][0]['object_identity']
            if mode=='other_node':row['after'][1]['object_identity']='changed'
            if mode=='dependency':row['after'][0]['dependencies']=[]
            if mode=='prior_clone':row['other_clone_unchanged']=False
            with self.subTest(mode=mode),self.assertRaises(ValueError):check_versions(r,g)

    def test_missing_and_duplicate_nodes_or_reloads(self):
        for mode in ['initial','hole','reload','duplicate']:
            r,g=self.fixture()
            if mode=='initial':r['initial_graph'].pop()
            if mode=='hole':r['unregistered_hole_graph'].pop()
            if mode=='reload':r['saved_payload_reloads'].pop()
            if mode=='duplicate':r['initial_graph'].append(copy.deepcopy(r['initial_graph'][0]))
            with self.subTest(mode=mode),self.assertRaises(ValueError):check_versions(r,g)

    def test_metadata_is_not_cloned_workload_acceptance(self):
        for field,value in [('completed',False),('closed',False),('observers_restored',False),
                            ('cloned_version_workload_playback',True),('cloned_version_resource_initialization',False),
                            ('default_pixels_equal',False),('observer_errors',['failed'])]:
            r,g=self.fixture();r[field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):check_versions(r,g)

    def test_equal_pixels_do_not_excuse_different_execution_counts(self):
        r,g=self.fixture();r['default_playbacks'][1]['audit']['event_dispatch_count']=8
        with self.assertRaises(ValueError):check_versions(r,g)
        r,g=self.fixture()
        for p in r['default_playbacks']:
            p['audit']['event_dispatch_count']=8;p['audit']['event_function_rvas']={'0x25f20':8}
        with self.assertRaises(ValueError):check_versions(r,g)

    def test_wrong_initial_edges(self):
        r,g=self.fixture();r['initial_graph'][1]['dependents']=[]
        with self.assertRaises(ValueError):check_versions(r,g)

    def primitive_fixture(self):
        def node(d=(),c=(),o=0,s=0):return dict(dependencies=list(d),dependents=list(c),object_token=o,status=s)
        cases=[]
        for kind in ['state','resource','erg','data']:
            deps=[11,22] if kind in ['erg','data'] else [11];updated=deps+[33];zero=[0] if kind=='erg' else []
            original=node(deps,[200],1,1);changed=node(updated,[200,201],2,1);emptied=node(zero+updated,[200,201],2,1)
            steps=[('fresh',{'0':node()}),('collected',{'0':original}),
                ('sparse_growth',{'0':original,'1':node(),'7':node()}),('copied',{'0':original,'7':original}),
                ('recollected',{'0':original,'7':changed}),('empty_recollection',{'0':original,'7':emptied}),
                ('fresh_empty',{'0':original,'1':node(zero,[],3,1),'7':emptied}),
                ('copy_replaces_sets',{'0':original,'1':node(zero,[],3,1),'7':original})]
            cases.append(dict(kind=kind,passed=True,steps=[dict(label=l,nodes=copy.deepcopy(n),expected=copy.deepcopy(n)) for l,n in steps]))
        return dict(completed=True,passed=True,player_sha256=PLAYER,gpu_execution=False,original_capture_acceptance=False,cases=cases)

    def test_native_primitive_scope(self):
        r=self.primitive_fixture();self.assertEqual(check_primitives(r)['transitions'],32)
        r['original_capture_acceptance']=True
        with self.assertRaises(ValueError):check_primitives(r)

    def test_removing_old_dependency_or_sharing_sets_is_not_verified(self):
        for mode in ['replace_dependencies','shared_dependents','default_object','empty_clears','copy_unions']:
            r=self.primitive_fixture();steps=r['cases'][0]['steps']
            if mode=='replace_dependencies':steps[4]['nodes']['7']['dependencies']=[33]
            if mode=='shared_dependents':steps[4]['nodes']['0']['dependents']=[200,201]
            if mode=='default_object':steps[4]['nodes']['0']['object_token']=2
            if mode=='empty_clears':steps[5]['nodes']['7']['dependencies']=[]
            if mode=='copy_unions':steps[7]['nodes']['7']['dependencies']=[11,33]
            for step in steps:step['expected']=copy.deepcopy(step['nodes'])
            with self.subTest(mode=mode),self.assertRaises(ValueError):check_primitives(r)


if __name__=='__main__':unittest.main()
