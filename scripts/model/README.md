# Static NR weight extraction

This MIT-licensed static extractor is copyright SamG-Coder; see LICENSE.
The small package.json only marks its JavaScript files as ES modules. No Node
packages are required. Node 20 or newer is recommended.

From the client source directory:

```sh
node scripts/model/scripts/extract-model.mjs /path/to/nvngx_dlssnr.dll models/nr
```

The output directory must not already exist. The extractor reads the DLL's PE
`WEIGHTS_HT` resource as bytes; it does not load or execute native code. Output
is `manifest.json` and eleven `model/stage-N.bin` files in the format consumed
by `nr::Model` in src/nr_model.cpp. The manifest records source/stage SHA-256
hashes, tensor offsets and byte lengths.

Supported local source: nvngx_dlssnr.dll, NR 310.8.0, SHA-256
`2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f`.
The expected model contains 71 blocks and 153 tensors, totalling 147,683,778
packed bytes. Keep DLLs and extracted weights local and separate from source
publication. The models directory is git ignored.
