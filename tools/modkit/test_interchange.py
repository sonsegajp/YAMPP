"""Procedural geometry regressions; no game data is required."""
import copy,json,struct,tempfile,unittest
from pathlib import Path
import numpy as np
from interchange import DTYPES,WIDTHS,export_model,export_stage,load_gltf,matrix,quaternion

def glb(path):
 raw=path.read_bytes();size=struct.unpack_from('<I',raw,12)[0];doc=json.loads(raw[20:20+size]);data=raw[28+size:]
 def access(index):
  a=doc['accessors'][index];v=doc['bufferViews'][a['bufferView']]
  return np.frombuffer(data,DTYPES[a['componentType']],a['count']*WIDTHS[a['type']],v.get('byteOffset',0)+a.get('byteOffset',0)).reshape(a['count'],-1).copy()
 return doc,access

def model():
 return {'bones':[{'id':0,'name':'root','parent':-1,'flags':0,'translation':[3,2,1],'rotation':[0,0,.3],'scale':[1,1,1]}],
 'meshes':[{'name':'split_seam','joint':0,'dobj':0,'positions':[[0,0,0],[1,0,0],[0,1,0],[0,0,0]],'normals':[[0,0,1],[0,0,1],[0,0,1],[0,.6,.8]],'texcoords':[[0,0],[1,0],[0,1],[.2,.3]],'indices':[0,2,1,3,2,1],'skin':[[0,0,0,0]]*4,'weights':[[1,0,0,0]]*4,'rigid':[False]*4}]}

class InterchangeTests(unittest.TestCase):
 def setUp(self):self.tmp=tempfile.TemporaryDirectory();self.path=Path(self.tmp.name)/'model.glb'
 def tearDown(self):self.tmp.cleanup()
 def test_native_clockwise_becomes_front_facing_without_changing_attributes(self):
  source=model();before=copy.deepcopy(source);export_model(source,self.path,source_winding='clockwise');doc,get=glb(self.path);p=doc['meshes'][0]['primitives'][0];m=source['meshes'][0]
  np.testing.assert_array_equal(get(p['indices']).ravel(),[0,1,2,3,1,2])
  for semantic,key in [('POSITION','positions'),('NORMAL','normals'),('TEXCOORD_0','texcoords'),('JOINTS_0','skin'),('WEIGHTS_0','weights')]:np.testing.assert_allclose(get(p['attributes'][semantic]),m[key],atol=1e-7)
  pos=get(p['attributes']['POSITION']);normal=get(p['attributes']['NORMAL'])
  for a,b,c in get(p['indices']).reshape(-1,3):self.assertGreater(np.dot(np.cross(pos[b]-pos[a],pos[c]-pos[a]),normal[[a,b,c]].mean(0)),0)
  self.assertEqual(source,before)
 def test_counterclockwise_callers_are_not_flipped(self):
  source=model();source['meshes'][0]['indices']=[0,1,2];export_model(source,self.path);doc,get=glb(self.path)
  np.testing.assert_array_equal(get(doc['meshes'][0]['primitives'][0]['indices']).ravel(),[0,1,2])
 def test_rigid_transform_preserves_authored_normal_direction(self):
  source=model();source['meshes'][0]['rigid']=[True]*4;export_model(source,self.path,source_winding='clockwise');doc,get=glb(self.path);p=doc['meshes'][0]['primitives'][0];b=source['bones'][0];w=matrix(dict(translation=b['translation'],rotation=quaternion(b['rotation']),scale=b['scale']))
  expected=(w@np.c_[source['meshes'][0]['positions'],np.ones(4)].T).T[:,:3];np.testing.assert_allclose(get(p['attributes']['POSITION']),expected,atol=1e-6)
  expected=(np.linalg.inv(w[:3,:3]).T@np.asarray(source['meshes'][0]['normals']).T).T;np.testing.assert_allclose(get(p['attributes']['NORMAL']),expected,atol=1e-6)
 def test_skin_and_animation_survive_winding_conversion(self):
  animation={'frames':1,'nodes':[[{'type':5,'values':[3,4]}]]};export_model(model(),self.path,[('Move',animation)],source_winding='clockwise');doc,get=glb(self.path)
  self.assertEqual(doc['skins'][0]['joints'],[0]);self.assertEqual(doc['animations'][0]['name'],'Move');sampler=doc['animations'][0]['samplers'][0]
  np.testing.assert_allclose(get(sampler['output']),[[3,2,1],[4,2,1]]);self.assertEqual(doc['nodes'][0]['extras']['melee_bone_id'],0)
 def test_stage_round_trip_keeps_gltf_winding(self):
  m=model()['meshes'][0];m['indices']=[0,1,2];export_stage({'meshes':[m]},self.path);loaded=load_gltf(self.path)['meshes'][0]
  self.assertEqual(loaded['indices'],[0,1,2]);np.testing.assert_allclose(loaded['normals'],m['normals'],atol=1e-6)
 def test_unknown_convention_rejected_before_output(self):
  with self.assertRaisesRegex(ValueError,'winding'):export_model(model(),self.path,source_winding='unknown')
  self.assertFalse(self.path.exists())

if __name__=='__main__':unittest.main()
