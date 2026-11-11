import importlib.util
import pathlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
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
 def test_invalid_and_missing_option_values(self):
  mod=self.module()
  for args in [['--model'],['--width','-1'],['--height','NaN'],['--unexpected','x'],['a.glb','b.hdr','c.glb']]:
   with self.assertRaises(ValueError,msg=str(args)):mod.native_arguments(pathlib.Path('/tmp'),args)
 def test_resources_missing(self):
  mod=self.module()
  with tempfile.TemporaryDirectory() as tmp:
   with self.assertRaisesRegex(ValueError,'build'):mod.check_resources(pathlib.Path(tmp),mod.native_arguments(pathlib.Path(tmp),[]))
 def test_launch_exit_code_and_paths_with_spaces(self):
  with tempfile.TemporaryDirectory(prefix='Native NR test ') as tmp:
   root=pathlib.Path(tmp)
   (root/'scripts').mkdir();shutil.copy(MODULE,root/'scripts/client_launch.py')
   files={'build/nr-doctor':'#!/bin/sh\nexit 0\n',
          'build/demo/dlss5-demo':'#!/bin/sh\nexit 17\n',
          'build/shaders/ops.spv':'test', 'assets/test-static.glb':'test',
          'build/data/materials/present.filamat':'test',
          'build/data/shaders/nr_preprocess.comp.spv':'test',
          'build/data/shaders/nr_composite.comp.spv':'test',
          'build/data/shaders/velocity_unpack.comp.spv':'test'}
   for relative,content in files.items():
    path=root/relative;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(content);path.chmod(0o755)
   model=root/'models/nr';model.mkdir(parents=True)
   (model/'manifest.json').write_text(json.dumps({'totals':{'blockCount':71},'tensors':[{}]*153,'stages':[]}))
   run=lambda:subprocess.run([sys.executable,str(root/'scripts/client_launch.py')],cwd='/',capture_output=True,text=True)
   self.assertEqual(run().returncode,17)
   (root/'build/nr-doctor').write_text('#!/bin/sh\nexit 2\n')
   self.assertEqual(run().returncode,2)
   (root/'build/nr-doctor').write_text('#!/bin/sh\nexit 0\n')
   (model/'manifest.json').unlink()
   result=run();self.assertEqual(result.returncode,1);self.assertIn('manifest.json',result.stderr)
if __name__=='__main__':unittest.main()
