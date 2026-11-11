import importlib.util
import pathlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import os
import hashlib
MODULE=pathlib.Path(__file__).parents[1]/'scripts/client_launch.py'
class LauncherTest(unittest.TestCase):
 def module(self):
  spec=importlib.util.spec_from_file_location('launcher',MODULE)
  mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod);return mod
 def test_paths_and_overrides_are_arguments(self):
  mod=self.module()
  with tempfile.TemporaryDirectory(prefix='NR space ') as tmp:
   root=pathlib.Path(tmp)
   args=mod.native_arguments(root,['/tmp/my model.glb','--model','/tmp/my weights','--width','512','--height','512'])
   self.assertEqual(args[0],str(root/'build/demo/dlss5-demo'))
   self.assertIn('/tmp/my model.glb',args)
   self.assertEqual(args[args.index('--model')+1],'/tmp/my weights')
   self.assertNotIn('--view',args)
   self.assertEqual(args[args.index('--width')+1],'512')
 def test_default_asset_and_size(self):
  mod=self.module();root=pathlib.Path('/tmp/client root')
  args=mod.native_arguments(root,[])
  self.assertIn(str(root/'assets/test-static.glb'),args)
  self.assertEqual(args[args.index('--width')+1],'1024')
  self.assertIn('--view',args)
 def test_environment_preset_passed_literally(self):
  args=self.module().native_arguments(pathlib.Path('/tmp/client root'),['--environment-preset','chalk-quarry-sunset'])
  self.assertEqual(args[args.index('--environment-preset')+1],'chalk-quarry-sunset')
 def test_invalid_and_missing_option_values(self):
  mod=self.module()
  for args in [['--model'],['--width','-1'],['--height','NaN'],['--unexpected','x'],['a.glb','b.hdr','c.glb']]:
   with self.assertRaises(ValueError,msg=str(args)):mod.native_arguments(pathlib.Path('/tmp'),args)
 def test_resources_missing(self):
  mod=self.module()
  with tempfile.TemporaryDirectory() as tmp:
   with self.assertRaisesRegex(ValueError,'build'):mod.check_resources(pathlib.Path(tmp),mod.native_arguments(pathlib.Path(tmp),[]))
 def fixture(self,root):
  files={'build/nr-doctor':'#!/bin/sh\nexit 0\n', 'build/demo/dlss5-demo':'#!/bin/sh\nexit 17\n',
         'assets/test-static.glb':'test', 'build/data/materials/present.filamat':'test'}
  shaders=('gemm_fp8','gemm_f16','ops','window_normalize','window_attend','global_normalize',
           'global_attend','preprocess','fused_block32','qkv_attention','gemm_mlp','global_attention','gemm_reduce')
  for name in shaders:files['build/shaders/'+name+'.spv']='test'
  for name in ('nr_preprocess','nr_composite','velocity_unpack'):files['build/data/shaders/'+name+'.comp.spv']='test'
  # Obtain the expected build outputs without executing emitters or creating build directories.
  spec=importlib.util.spec_from_file_location('generate',MODULE.parent/'linux/generate.py')
  gen=importlib.util.module_from_spec(spec);spec.loader.exec_module(gen)
  with patch.object(gen,'emit',side_effect=lambda name,*args:files.update({str(arg.relative_to(gen.BUILD)):'test' for arg in args if isinstance(arg,pathlib.Path)})):
   with patch.object(pathlib.Path,'mkdir'):gen.ptx()
  files={('build/'+key if key.startswith('ptx/') else key):value for key,value in files.items()}
  for relative,content in files.items():
   path=root/relative;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(content);path.chmod(0o755)
  model=root/'models/nr';(model/'model').mkdir(parents=True)
  stages=[]
  for index in range(11):
   name=f'stage-{index}.bin';(model/'model'/name).write_bytes(b'\0'*16)
   stages.append({'file':name,'packedByteLength':16,'id':index,'sha256':hashlib.sha256(b'\0'*16).hexdigest()})
  (model/'manifest.json').write_text(json.dumps({'totals':{'blockCount':71},'tensors':[{'name':f'tensor-{i}','block':i%71,'layer':'test','parameter':'weight','stage':i%11,'stageOffset':0,'byteLength':16} for i in range(153)],'stages':stages}))
  return model
 def test_all_required_modules_checked(self):
  mod=self.module()
  for missing in (name+'.spv' for name in mod._resources.SPIRV_MODULES):
   with self.subTest(missing=missing),tempfile.TemporaryDirectory() as tmp:
    root=pathlib.Path(tmp);self.fixture(root);(root/'build/shaders'/missing).unlink()
    with self.assertRaisesRegex(ValueError,missing):mod.check_resources(root,mod.native_arguments(root,[]))
 def test_ptx_and_custom_kernel_directory(self):
  mod=self.module()
  with tempfile.TemporaryDirectory() as tmp,patch.dict(os.environ,{},clear=True):
   root=pathlib.Path(tmp);self.fixture(root)
   custom=root/'custom kernels';shutil.copytree(root/'build/shaders',custom)
   override=root/'custom PTX';shutil.copytree(root/'build/ptx',override)
   args=mod.native_arguments(root,['--kernels',str(custom)])
   with self.assertRaisesRegex(ValueError,'ptx'):mod.check_resources(root,args)
   with patch.dict(os.environ,{'DLSS5VK_PTX_DIR':str(override)}):
    mod.check_resources(root,args)
    (override/'block32_e4m3_f130.ptx').unlink()
    with self.assertRaisesRegex(ValueError,'block32_e4m3_f130.ptx'):mod.check_resources(root,args)
 def test_malformed_manifest_and_empty_stages(self):
  mod=self.module()
  with tempfile.TemporaryDirectory() as tmp:
   root=pathlib.Path(tmp);model=self.fixture(root);path=model/'manifest.json';valid=json.loads(path.read_text())
   for manifest in ([],dict(valid,stages=[]),dict(valid,stages={}),dict(valid,totals=[]),dict(valid,tensors={}),dict(valid,stages=[None]*11)):
    path.write_text(json.dumps(manifest))
    with self.subTest(manifest=manifest),self.assertRaisesRegex(ValueError,'[Mm]odel|[Mm]anifest'):mod.check_resources(root,mod.native_arguments(root,[]))
 def test_launch_exit_code_and_paths_with_spaces(self):
  with tempfile.TemporaryDirectory(prefix='Native NR test ') as tmp:
   root=pathlib.Path(tmp)
   (root/'scripts/linux').mkdir(parents=True);shutil.copy(MODULE,root/'scripts/client_launch.py');shutil.copy(MODULE.parent/'linux/kernel_resources.py',root/'scripts/linux/kernel_resources.py')
   model=self.fixture(root)
   run=lambda:subprocess.run([sys.executable,str(root/'scripts/client_launch.py')],cwd='/',capture_output=True,text=True)
   self.assertEqual(run().returncode,17)
   (root/'build/nr-doctor').write_text('#!/bin/sh\nexit 2\n')
   self.assertEqual(run().returncode,2)
   (root/'build/nr-doctor').write_text('#!/bin/sh\nexit 0\n')
   (model/'manifest.json').unlink()
   result=run();self.assertEqual(result.returncode,1);self.assertIn('manifest.json',result.stderr)
if __name__=='__main__':unittest.main()
