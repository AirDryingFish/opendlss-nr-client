#!/usr/bin/env python3
"""Bake a non-animated, non-morph GLB's current skin pose for static import.

Requires NumPy. Keeps mesh node transforms; uses inverse(mesh world) * joint
world * inverse-bind for positions. Normals follow Filament's per-bone
normalized cofactor transform. Tangents are transformed as directions, then
orthogonalized against the baked normal, retaining their handedness.
"""
import argparse
import copy
import json
from pathlib import Path
import struct
import sys

try:
    import numpy as np
except ImportError:
    raise SystemExit('Static baking requires NumPy; use a Python environment with numpy installed.')

DTYPES = {5120: 'i1', 5121: 'u1', 5122: '<i2', 5123: '<u2', 5125: '<u4', 5126: '<f4'}
WIDTHS = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


def load_glb(path):
    data = Path(path).read_bytes()
    if len(data) < 20 or struct.unpack_from('<III', data) != (0x46546C67, 2, len(data)):
        raise ValueError('Expected a complete GLB version 2 file')
    chunks, offset = {}, 12
    while offset < len(data):
        size, kind = struct.unpack_from('<II', data, offset)
        offset += 8
        if size % 4 or offset + size > len(data) or kind in chunks:
            raise ValueError('Invalid GLB chunk')
        chunks[kind] = data[offset:offset + size]
        offset += size
    doc = json.loads(chunks[0x4E4F534A])
    if len(doc.get('buffers', [])) != 1 or 'uri' in doc['buffers'][0]:
        raise ValueError('Expected a single embedded GLB buffer')
    return doc, bytearray(chunks.get(0x004E4942, b''))


def read_accessor(doc, binary, index):
    a = doc['accessors'][index]
    dtype, width = np.dtype(DTYPES[a['componentType']]), WIDTHS[a['type']]
    def read(view_index, offset, count, columns, value_type, stride=None):
        view = doc['bufferViews'][view_index]
        step = stride or columns * value_type.itemsize
        length = (count - 1) * step + columns * value_type.itemsize if count else 0
        if view.get('buffer', 0) != 0 or offset + length > view['byteLength']:
            raise ValueError('Accessor exceeds its buffer view')
        return np.ndarray((count, columns), dtype=value_type, buffer=binary,
                          offset=view.get('byteOffset', 0) + offset,
                          strides=(step, value_type.itemsize)).copy()
    if 'bufferView' in a:
        view = doc['bufferViews'][a['bufferView']]
        values = read(a['bufferView'], a.get('byteOffset', 0), a['count'], width, dtype, view.get('byteStride'))
    else:
        values = np.zeros((a['count'], width), dtype=dtype)
    if 'sparse' in a:
        sparse = a['sparse']
        ind, val = sparse['indices'], sparse['values']
        indices = read(ind['bufferView'], ind.get('byteOffset', 0), sparse['count'], 1,
                       np.dtype(DTYPES[ind['componentType']])).ravel()
        values[indices] = read(val['bufferView'], val.get('byteOffset', 0), sparse['count'], width, dtype)
    values = values.astype(np.float64)
    if a.get('normalized') and a['componentType'] != 5126:
        limits = np.iinfo(dtype)
        values = np.maximum(values / limits.max, -1) if limits.min < 0 else values / limits.max
    return values


def local_matrix(node):
    if 'matrix' in node:
        return np.array(node['matrix'], dtype=np.float64).reshape(4, 4).T
    x, y, z, w = node.get('rotation', [0, 0, 0, 1])
    rotation = np.array([[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
                         [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
                         [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]])
    matrix = np.eye(4)
    matrix[:3, :3] = rotation @ np.diag(node.get('scale', [1, 1, 1]))
    matrix[:3, 3] = node.get('translation', [0, 0, 0])
    return matrix


def world_matrices(doc):
    parents = {}
    for index, node in enumerate(doc['nodes']):
        for child in node.get('children', []):
            if child in parents:
                raise ValueError('Node has multiple parents')
            parents[child] = index
    worlds, active = {}, set()
    def visit(index):
        if index in active:
            raise ValueError('Node hierarchy contains a cycle')
        if index not in worlds:
            active.add(index)
            worlds[index] = (visit(parents[index]) if index in parents else np.eye(4)) @ local_matrix(doc['nodes'][index])
            active.remove(index)
        return worlds[index]
    for index in range(len(doc['nodes'])):
        visit(index)
    return worlds, parents


def unit(values):
    lengths = np.linalg.norm(values, axis=-1, keepdims=True)
    if np.any(lengths < 1e-20):
        raise ValueError('Cannot normalize a zero normal or tangent')
    return values / lengths


def bake(doc, binary):
    doc = copy.deepcopy(doc)
    if doc.get('animations'):
        raise ValueError('Animation-bearing assets cannot be statically baked')
    if any(p.get('targets') for m in doc.get('meshes', []) for p in m['primitives']):
        raise ValueError('Morph-bearing assets cannot be statically baked')
    if any('extensions' in p for m in doc.get('meshes', []) for p in m['primitives']):
        raise ValueError('Compressed or extended primitives are not supported by this baker')
    worlds, parents = world_matrices(doc)
    def append(values, kind):
        values = np.asarray(values, dtype='<f4')
        if not np.isfinite(values).all():
            raise ValueError('Baked vertex data is not finite')
        binary.extend(b'\0' * (-len(binary) % 4))
        doc['bufferViews'].append({'buffer': 0, 'byteOffset': len(binary), 'byteLength': values.nbytes, 'target': 34962})
        binary.extend(values.tobytes())
        a = {'bufferView': len(doc['bufferViews']) - 1, 'componentType': 5126,
             'count': len(values), 'type': kind}
        if kind == 'VEC3':
            a.update(min=values.min(axis=0).tolist(), max=values.max(axis=0).tolist())
        doc['accessors'].append(a)
        return len(doc['accessors']) - 1
    for index, node in enumerate(doc['nodes']):
        if 'skin' not in node:
            continue
        skin = doc['skins'][node['skin']]
        joint_count = len(skin['joints'])
        ibms = (read_accessor(doc, binary, skin['inverseBindMatrices']).reshape(-1, 4, 4).transpose(0, 2, 1)
                if 'inverseBindMatrices' in skin else np.repeat(np.eye(4)[None], joint_count, axis=0))
        matrices = np.array([np.linalg.inv(worlds[index]) @ worlds[j] @ ibms[k]
                             for k, j in enumerate(skin['joints'])])
        linear = matrices[:, :3, :3]
        # Column-wise cross products avoid an inverse and support singular transforms.
        cofactors = np.stack([np.cross(linear[:, :, 1], linear[:, :, 2]),
                              np.cross(linear[:, :, 2], linear[:, :, 0]),
                              np.cross(linear[:, :, 0], linear[:, :, 1])], axis=2)
        mesh = copy.deepcopy(doc['meshes'][node['mesh']])
        doc['meshes'].append(mesh)
        node['mesh'] = len(doc['meshes']) - 1
        for primitive in mesh['primitives']:
            attr = primitive['attributes']
            sets = sorted(k[7:] for k in attr if k.startswith('JOINTS_'))
            if not sets or any('WEIGHTS_' + s not in attr for s in sets):
                raise ValueError('Skinned primitive must have matched joints and weights')
            joints = np.concatenate([read_accessor(doc, binary, attr['JOINTS_' + s]) for s in sets], axis=1).astype(np.int64)
            weights = np.concatenate([read_accessor(doc, binary, attr['WEIGHTS_' + s]) for s in sets], axis=1)
            if np.any(weights < 0) or np.any(weights.sum(axis=1) <= 0):
                raise ValueError('Skin weights must be nonnegative with a positive sum')
            if np.any(joints < 0) or np.any(joints >= joint_count):
                raise ValueError('Joint index exceeds skin palette')
            weights /= weights.sum(axis=1, keepdims=True)
            position = read_accessor(doc, binary, attr['POSITION'])
            result = np.zeros_like(position)
            normal = read_accessor(doc, binary, attr['NORMAL']) if 'NORMAL' in attr else None
            tangent = read_accessor(doc, binary, attr['TANGENT']) if 'TANGENT' in attr else None
            out_normal = np.zeros_like(normal) if normal is not None else None
            out_tangent = np.zeros_like(tangent[:, :3]) if tangent is not None else None
            for slot in range(joints.shape[1]):
                chosen, weight = joints[:, slot], weights[:, slot, None]
                result += weight * (np.einsum('nij,nj->ni', linear[chosen], position) + matrices[chosen, :3, 3])
                active = weights[:, slot] > 0
                if normal is not None:
                    transformed = np.einsum('nij,nj->ni', cofactors[chosen[active]], normal[active])
                    out_normal[active] += weight[active] * unit(transformed)
                if tangent is not None:
                    transformed = np.einsum('nij,nj->ni', linear[chosen[active]], tangent[active, :3])
                    out_tangent[active] += weight[active] * unit(transformed)
            attr['POSITION'] = append(result, 'VEC3')
            if normal is not None:
                out_normal = unit(out_normal)
                attr['NORMAL'] = append(out_normal, 'VEC3')
            if tangent is not None:
                if normal is not None:
                    out_tangent -= out_normal * np.sum(out_normal * out_tangent, axis=1, keepdims=True)
                attr['TANGENT'] = append(np.column_stack([unit(out_tangent), tangent[:, 3]]), 'VEC4')
            for name in list(attr):
                if name.startswith(('JOINTS_', 'WEIGHTS_')):
                    del attr[name]
        del node['skin']
    # Drop only unused skeleton nodes, keeping every ancestor of retained content.
    skeleton = set()
    for skin in doc.pop('skins', []):
        for index in skin['joints']:
            while index not in skeleton:
                skeleton.add(index)
                if index not in parents:
                    break
                index = parents[index]
    keep = {i for i, node in enumerate(doc['nodes'])
            if i not in skeleton or any(key in node for key in ('mesh', 'camera', 'extensions'))}
    for index in list(keep):
        while index in parents:
            index = parents[index]
            keep.add(index)
    remap = {old: new for new, old in enumerate(sorted(keep))}
    doc['nodes'] = [n for i, n in enumerate(doc['nodes']) if i in keep]
    for node in doc['nodes']:
        if 'children' in node:
            node['children'] = [remap[i] for i in node['children'] if i in keep]
            if not node['children']:
                del node['children']
    for scene in doc.get('scenes', []):
        scene['nodes'] = [remap[i] for i in scene.get('nodes', []) if i in keep]
    mesh_ids = sorted({n['mesh'] for n in doc['nodes'] if 'mesh' in n})
    mesh_map = {old: new for new, old in enumerate(mesh_ids)}
    doc['meshes'] = [doc['meshes'][i] for i in mesh_ids]
    for node in doc['nodes']:
        if 'mesh' in node:
            node['mesh'] = mesh_map[node['mesh']]
    # Compact unused skin / original vertex accessors and views, preserving image bytes.
    used = set()
    for mesh in doc['meshes']:
        for primitive in mesh['primitives']:
            used.update(primitive['attributes'].values())
            if 'indices' in primitive:
                used.add(primitive['indices'])
    accessor_map = {old: new for new, old in enumerate(sorted(used))}
    doc['accessors'] = [doc['accessors'][i] for i in sorted(used)]
    for mesh in doc['meshes']:
        for p in mesh['primitives']:
            p['attributes'] = {k: accessor_map[v] for k, v in p['attributes'].items()}
            if 'indices' in p:
                p['indices'] = accessor_map[p['indices']]
    views = {a['bufferView'] for a in doc['accessors'] if 'bufferView' in a}
    for a in doc['accessors']:
        if 'sparse' in a:
            views.update(a['sparse'][k]['bufferView'] for k in ('indices', 'values'))
    views.update(i['bufferView'] for i in doc.get('images', []) if 'bufferView' in i)
    view_map = {old: new for new, old in enumerate(sorted(views))}
    output = bytearray()
    for old in sorted(views):
        view = doc['bufferViews'][old]
        start = view.get('byteOffset', 0)
        output.extend(b'\0' * (-len(output) % 4))
        chunk = binary[start:start + view['byteLength']]
        view['byteOffset'] = len(output)
        output.extend(chunk)
    doc['bufferViews'] = [doc['bufferViews'][i] for i in sorted(views)]
    for a in doc['accessors']:
        if 'bufferView' in a:
            a['bufferView'] = view_map[a['bufferView']]
        if 'sparse' in a:
            for k in ('indices', 'values'):
                a['sparse'][k]['bufferView'] = view_map[a['sparse'][k]['bufferView']]
    for image in doc.get('images', []):
        if 'bufferView' in image:
            image['bufferView'] = view_map[image['bufferView']]
    doc['buffers'][0]['byteLength'] = len(output)
    return doc, output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        parser.error('Output must differ from the original input')
    try:
        doc, binary = load_glb(args.input)
        doc, binary = bake(doc, binary)
        raw = json.dumps(doc, separators=(',', ':')).encode()
        raw += b' ' * (-len(raw) % 4)
        binary.extend(b'\0' * (-len(binary) % 4))
        payload = (struct.pack('<III', 0x46546C67, 2, 28 + len(raw) + len(binary)) +
                   struct.pack('<II', len(raw), 0x4E4F534A) + raw +
                   struct.pack('<II', len(binary), 0x004E4942) + binary)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(payload)
        print(f'Baked {len(doc["meshes"])} meshes to {args.output} ({len(payload)} bytes)')
    except (ValueError, KeyError, IndexError, np.linalg.LinAlgError) as error:
        parser.exit(1, f'Cannot bake GLB: {error}\n')


if __name__ == '__main__':
    main()
