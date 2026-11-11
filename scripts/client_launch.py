#!/usr/bin/env python3
"""Launch the native renderer with checked resources and literal argument vectors."""
import json
import importlib.util
import os
from pathlib import Path
import subprocess
import sys

# This module is a pure manifest: importing it never invokes shader/PTX generators.
_spec = importlib.util.spec_from_file_location('kernel_resources', Path(__file__).parent / 'linux/kernel_resources.py')
_resources = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_resources)

VALUE_FLAGS = {
    '--model', '--kernels', '--scenes', '--capture', '--view', '--width', '-w',
    '--height', '-h', '--frames', '--orbit', '--animation', '--temporal', '--nr',
    '--intensity', '--style', '--style-knobs', '--benchmark-frames',
    '--benchmark-warmup', '--benchmark-output',
}


def native_arguments(root, arguments):
    flags, positional = {}, []
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value in VALUE_FLAGS:
            if index + 1 >= len(arguments):
                raise ValueError('Missing value for ' + value)
            flags[value] = arguments[index + 1]
            index += 2
        elif value.startswith('-'):
            raise ValueError('Unknown option: ' + value)
        else:
            positional.append(value)
            index += 1
    if len(positional) > 2:
        raise ValueError('Choose one GLB/glTF and optional HDR environment.')
    default_scene = not positional
    if default_scene:
        positional.append(str(root / 'assets/test-static.glb'))
    if Path(positional[0]).suffix.lower() not in {'.glb', '.gltf'}:
        raise ValueError('The scene must be a .glb or .gltf file.')
    if '-w' in flags:
        flags['--width'] = flags.pop('-w')
    if '-h' in flags:
        flags['--height'] = flags.pop('-h')
    flags.setdefault('--model', os.environ.get('DLSS5VK_MODEL', str(root / 'models/nr')))
    flags.setdefault('--kernels', str(root / 'build/shaders'))
    flags.setdefault('--scenes', str(root / 'build/scenes'))
    flags.setdefault('--width', '1024')
    flags.setdefault('--height', '1024')
    for name in ('--width', '--height'):
        try:
            size = int(flags[name])
        except ValueError:
            raise ValueError(name + ' must be an integer.') from None
        if not 64 <= size <= 8192:
            raise ValueError(name + ' must be between 64 and 8192 pixels.')
    if default_scene:
        flags.setdefault('--view', '0.22,1.58,0.88,0,1.48,0.06,40')
    result = [str(root / 'build/demo/dlss5-demo'), *positional]
    for key, value in flags.items():
        result.extend((key, value))
    return result


def check_resources(root, arguments):
    binary = Path(arguments[0])
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError('Native renderer is not built. Run scripts/linux/build.sh first.')
    directory = Path(arguments[arguments.index('--model') + 1])
    manifest_path = directory / 'manifest.json'
    if not manifest_path.is_file():
        raise ValueError('Missing ' + str(manifest_path))
    with manifest_path.open(encoding='utf8') as stream:
        manifest = json.load(stream)
    if (not isinstance(manifest, dict) or not isinstance(manifest.get('totals'), dict)
            or manifest['totals'].get('blockCount') != 71
            or not isinstance(manifest.get('tensors'), list) or len(manifest['tensors']) != 153
            or not all(isinstance(tensor, dict) and tensor for tensor in manifest['tensors'])
            or not isinstance(manifest.get('stages'), list) or len(manifest['stages']) != 11):
        raise ValueError('Incompatible model manifest: expected 71 blocks, 153 tensors and 11 stages.')
    for stage in manifest['stages']:
        if (not isinstance(stage, dict) or not isinstance(stage.get('file'), str) or not stage['file']
                or type(stage.get('packedByteLength')) is not int or stage['packedByteLength'] <= 0):
            raise ValueError('Incompatible model stage manifest.')
        target = (directory / 'model' / stage['file']).resolve()
        if not target.is_relative_to(directory.resolve() / 'model'):
            raise ValueError('Model stage path escapes model directory.')
        if not target.is_file() or target.stat().st_size != stage['packedByteLength']:
            raise ValueError('Missing or truncated model stage: ' + str(target))
    kernels = Path(arguments[arguments.index('--kernels') + 1])
    required = [kernels / (name + '.spv') for name in _resources.SPIRV_MODULES]
    ptx = Path(os.environ.get('DLSS5VK_PTX_DIR', str(kernels / '../ptx')))
    required.extend(arg for job in _resources.ptx_jobs(ptx) for arg in job if isinstance(arg, Path))
    for resource in required:
        if not resource.is_file():
            raise ValueError('Missing kernel resource: ' + str(resource))
    # Native code does the stage SHA-256 verification; avoid a second full hash pass here.
    for resource in ('build/data/materials/present.filamat',
                     'build/data/shaders/nr_preprocess.comp.spv',
                     'build/data/shaders/nr_composite.comp.spv',
                     'build/data/shaders/velocity_unpack.comp.spv'):
        if not (root / resource).is_file():
            raise ValueError('Missing renderer resource: ' + resource)
    scene = Path(arguments[1])
    if not scene.is_file():
        raise ValueError('Scene does not exist: ' + str(scene))


def main(arguments=None):
    arguments = sys.argv[1:] if arguments is None else arguments
    root = Path(__file__).resolve().parents[1]
    if '--help' in arguments:
        print('Usage: scripts/run-client.sh [model.glb [environment.hdr]] [native options]\n'
              'Defaults: prepared test model, local NR weights, initial window 1024x1024.\n'
              'Render/NR resolution follows the actual window size; --width/--height request its initial size.\n'
              'Check only: scripts/check-gpu.sh [--json]\n'
              'Options: --model DIR --width N --height N --nr 0|1 --temporal 0|1\n'
              'Benchmark: --benchmark-warmup 30 --benchmark-frames 120 --benchmark-output result.json')
        return 0
    environment = os.environ.copy()
    environment['SDL_VIDEODRIVER'] = 'x11'
    environment['LD_LIBRARY_PATH'] = str(root / 'build/lib') + (':' + environment['LD_LIBRARY_PATH'] if environment.get('LD_LIBRARY_PATH') else '')
    doctor = root / 'build/nr-doctor'
    if not doctor.is_file():
        print('GPU checker is not built. Run scripts/linux/build.sh first.', file=sys.stderr)
        return 1
    try:
        result = subprocess.run([str(doctor)], env=environment, check=False)
        if result.returncode:
            return result.returncode
        native = native_arguments(root, arguments)
        check_resources(root, native)
        os.execve(native[0], native, environment)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print('Cannot start native NR: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
