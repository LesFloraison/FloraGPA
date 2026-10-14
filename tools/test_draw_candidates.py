"""CPU geometry and malformed-input controls for the development candidate search."""
import copy
import hashlib
import struct
import unittest
from inspect_draw_candidates import project


def fixture(points):
    raw = b''.join(struct.pack('<4f', *p) for p in points)
    report = dict(completed=True,frame_sha256='frame',event=1,shader_sha256='shader',
                  vertex_count=len(points),primitives=len(points)//3,stride=16,
                  bytes_sha256=hashlib.sha256(raw).hexdigest(),
                  attributes=[dict(semantic='SV_POSITION',index=0,component_type=3,count=4,
                                   start=0,offset=0,never_written_mask=0)])
    bundle = dict(completed=True,frame_sha256='frame',event=1,vs=dict(shader=dict(sha256='shader')),
                  topology=4,draw=[len(points),0,0],
                  viewports=[list(struct.unpack('<6I',struct.pack('<6f',0,0,2,2,0,1)))])
    return report,raw,bundle


class ProjectionTests(unittest.TestCase):
    def setUp(self):
        self.args = fixture([(-1,1,0,1),(1,1,0,1),(-1,-1,0,1)])

    def test_independent_geometry(self):
        c = project(*self.args,0,0,sample_offsets=[(0.25,0.25)])['candidates'][0]
        self.assertEqual(c['screen'],[[0,0],[2,0],[0,2]])
        self.assertEqual(c['area'],2)
        self.assertEqual(c['barycentric']['center'],[0.5,0.25,0.25])
        self.assertEqual(c['barycentric']['sample-0'],[0.25,0.375,0.375])

    def test_search_is_not_coverage(self):
        c = project(*self.args,1,1,radius=0)['candidates'][0]
        self.assertEqual(c['barycentric']['center'],[-0.5,0.75,0.75])
        self.assertEqual(project(*self.args,10,10,radius=0)['candidates'],[])

    def test_degenerate_retained_without_invented_weights(self):
        args=fixture([(0,0,0,1)]*3)
        c=project(*args,0,0)['candidates'][0]
        self.assertEqual(c['area'],0)
        self.assertIsNone(c['barycentric']['center'])

    def test_perspective_divide_and_viewport_offset(self):
        args=list(fixture([(-2,2,0,2),(4,4,0,4),(-8,-8,0,8)]))
        args[2]['viewports']=[list(struct.unpack('<6I',struct.pack('<6f',3,5,4,6,0,1)))]
        self.assertEqual(project(*args,3,5)['candidates'][0]['screen'],[[3,5],[7,5],[3,11]])

    def test_unprojectable_vertices_fail(self):
        for w in (0,-1,float('nan')):
            with self.subTest(w=w),self.assertRaisesRegex(ValueError,'clip W'):
                project(*fixture([(0,0,0,w)]*3),0,0)

    def test_input_identity_storage_and_signature(self):
        for key,value in [('event',2),('completed',False),('bytes_sha256','wrong'),
                          ('stride',15),('vertex_count',2),('primitives',3)]:
            args=copy.deepcopy(self.args);args[0][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):project(*args,0,0)
        for key,value in [('offset',4),('count',3),('never_written_mask',1),('component_type',1)]:
            args=copy.deepcopy(self.args);args[0]['attributes'][0][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):project(*args,0,0)
        with self.assertRaises(ValueError):project(self.args[0],self.args[1][:-1],self.args[2],0,0)

    def test_pixel_viewport_and_sample_bounds(self):
        for options in [dict(x=-1,y=0),dict(x=True,y=0),dict(x=0,y=0,radius=float('nan')),
                        dict(x=0,y=0,radius=-1),dict(x=0,y=0,sample_offsets=[(1,0)])]:
            with self.subTest(options=options),self.assertRaises(ValueError):project(*self.args,**options)
        args=copy.deepcopy(self.args);args[2]['viewports']*=2
        with self.assertRaisesRegex(ValueError,'one encoded viewport'):project(*args,0,0)


if __name__ == '__main__':
    unittest.main()
