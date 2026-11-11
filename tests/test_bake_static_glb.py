"""CLI tests: catch lost mesh scale, unnormalized weights and wrong normal transforms."""
import copy
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/bake_static_glb.py'


def write_fixture(path, animation=False, morph=False):
    data = bytearray()
    doc = {'asset': {'version': '2.0'}, 'buffers': [{}], 'bufferViews': [], 'accessors': [],
           'materials': [{'name': 'Face', 'alphaMode': 'OPAQUE'}],
           'nodes': [{'translation': [10, 0, 0], 'scale': [.5, .5, .5], 'children': [1]},
                     {'scale': [.02, .02, .02], 'mesh': 0, 'skin': 0},
                     {'translation': [1, 0, 0], 'scale': [2, 2, 2], 'children': [3, 4]},
                     {'scale': [50, 100, 150]}, {'scale': [100, 50, 150]}],
           'scenes': [{'nodes': [0, 2]}], 'scene': 0, 'meshes': []}
    def accessor(values, fmt, count, kind, component):
        while len(data) % 4: data.append(0)
        raw = struct.pack('<' + fmt, *values)
        doc['bufferViews'].append({'buffer': 0, 'byteOffset': len(data), 'byteLength': len(raw)})
        doc['accessors'].append({'bufferView': len(doc['bufferViews']) - 1,
                                 'componentType': component, 'count': count, 'type': kind})
        data.extend(raw)
        return len(doc['accessors']) - 1
    position = accessor([1, 1, 0, 0, 0, 0, 1, 0, 0], '9f', 3, 'VEC3', 5126)
    normal = accessor([1 / math.sqrt(2), 1 / math.sqrt(2), 0] * 3, '9f', 3, 'VEC3', 5126)
    tangent = accessor([1 / math.sqrt(2), -1 / math.sqrt(2), 0, -1] * 3, '12f', 3, 'VEC4', 5126)
    joints = accessor([0, 1, 0, 0] * 3, '12H', 3, 'VEC4', 5123)
    weights = accessor([.5, 1.5, 0, 0] * 3, '12f', 3, 'VEC4', 5126)
    ibm = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, .1, 0, 0, 1]
    inverse_bind = accessor(ibm * 2, '32f', 2, 'MAT4', 5126)
    primitive = {'attributes': {'POSITION': position, 'NORMAL': normal, 'TANGENT': tangent,
                                'JOINTS_0': joints, 'WEIGHTS_0': weights}, 'material': 0}
    if morph: primitive['targets'] = [{'POSITION': position}]
    if animation: doc['animations'] = [{'channels': [], 'samplers': []}]
    doc['meshes'] = [{'primitives': [primitive]}]
    doc['skins'] = [{'joints': [3, 4], 'inverseBindMatrices': inverse_bind}]
    doc['buffers'][0]['byteLength'] = len(data)
    raw = json.dumps(doc).encode()
    raw += b' ' * (-len(raw) % 4)
    data.extend(b'\0' * (-len(data) % 4))
    path.write_bytes(struct.pack('<III', 0x46546c67, 2, 28 + len(raw) + len(data)) +
                     struct.pack('<II', len(raw), 0x4e4f534a) + raw +
                     struct.pack('<II', len(data), 0x004e4942) + data)
    return doc


def read_output(path):
    data = path.read_bytes()
    size = struct.unpack_from('<I', data, 12)[0]
    doc = json.loads(data[20:20 + size])
    binary = data[28 + size:]
    def values(index):
        a = doc['accessors'][index]
        v = doc['bufferViews'][a['bufferView']]
        width = {'VEC3': 3, 'VEC4': 4}[a['type']]
        return struct.unpack_from('<' + str(a['count'] * width) + 'f', binary,
                                  v.get('byteOffset', 0) + a.get('byteOffset', 0))
    return doc, values


class BakeTests(unittest.TestCase):
    def run_bake(self, source, output):
        return subprocess.run([sys.executable, str(SCRIPT), str(source), str(output)], capture_output=True, text=True)

    def test_preserves_world_scale_and_filament_normals(self):
        with tempfile.TemporaryDirectory() as folder:
            source, output = Path(folder) / 'source.glb', Path(folder) / 'static.glb'
            original = write_fixture(source)
            result = self.run_bake(source, output)
            self.assertEqual(result.returncode, 0, result.stderr)
            doc, values = read_output(output)
            node = next(n for n in doc['nodes'] if 'mesh' in n)
            self.assertEqual(node['scale'], [.02, .02, .02])
            attr = doc['meshes'][node['mesh']]['primitives'][0]['attributes']
            # Hand derived: .25*(100*1+11)+.75*(200*1+21) = 193.5 world X.
            p = values(attr['POSITION'])
            self.assertAlmostEqual(10 + .01 * p[0], 193.5, places=4)
            self.assertAlmostEqual(.01 * p[1], 125, places=4)
            n = values(attr['NORMAL'])
            for got, want in zip(n[:3], [5 / math.sqrt(74), 7 / math.sqrt(74), 0]):
                self.assertAlmostEqual(got, want, places=6)
            t = values(attr['TANGENT'])
            for got, want in zip(t[:4], [7 / math.sqrt(74), -5 / math.sqrt(74), 0, -1]):
                self.assertAlmostEqual(got, want, places=6)
            self.assertEqual(doc['materials'], original['materials'])
            self.assertNotIn('skins', doc)
            self.assertFalse(any('skin' in n for n in doc['nodes']))
            self.assertFalse(any(k.startswith(('JOINTS_', 'WEIGHTS_')) for k in attr))
            self.assertEqual(len(doc['nodes']), 2)  # Keep mesh parent, discard joint subtree.
            pa = doc['accessors'][attr['POSITION']]
            self.assertEqual(pa['min'], [850.0, 0.0, 0.0])
            self.assertEqual(pa['max'], [18350.0, 12500.0, 0.0])
            self.assertTrue(all(v.get('byteOffset', 0) % 4 == 0 for v in doc['bufferViews']))

    def test_rejects_hardlinked_output_without_changing_original(self):
        with tempfile.TemporaryDirectory() as folder:
            source, output = Path(folder) / 'source.glb', Path(folder) / 'alias.glb'
            write_fixture(source)
            original = source.read_bytes()
            output.hardlink_to(source)
            self.assertTrue(source.samefile(output))
            result = self.run_bake(source, output)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertEqual(source.read_bytes(), original)
            self.assertEqual(hashlib.sha256(source.read_bytes()).digest(),
                             hashlib.sha256(original).digest())
            self.assertEqual(output.read_bytes(), original)

    def test_rejects_animation_without_writing_output(self):
        with tempfile.TemporaryDirectory() as folder:
            source, output = Path(folder) / 'source.glb', Path(folder) / 'static.glb'
            write_fixture(source, animation=True)
            result = self.run_bake(source, output)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('animation', result.stderr.lower())
            self.assertFalse(output.exists())

    def test_rejects_morph_without_writing_output(self):
        with tempfile.TemporaryDirectory() as folder:
            source, output = Path(folder) / 'source.glb', Path(folder) / 'static.glb'
            write_fixture(source, morph=True)
            result = self.run_bake(source, output)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('morph', result.stderr.lower())
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
