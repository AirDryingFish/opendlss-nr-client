"""Pure native kernel resource manifest shared by generation and startup checks."""
SPIRV_MODULES = (
    'gemm_fp8', 'gemm_f16', 'ops', 'window_normalize', 'window_attend',
    'global_normalize', 'global_attend', 'preprocess', 'fused_block32',
    'qkv_attention', 'gemm_mlp', 'global_attention', 'gemm_reduce',
)

def ptx_jobs(directory):
    for k in (64, 128, 256):
        yield ('mlp_e4m3', k, directory / f'mlp_e4m3_K{k}.ptx', 3, 1, 1)
    for k in (64, 128, 256, 512):
        yield ('qkv_e4m3', k, directory / f'qkv_e4m3_K{k}.ptx', 80)
    for k in (32, 64, 128, 256, 512, 1024):
        for f in (5, 13, 4, 8, 6):
            yield ('gemm2_e4m3', k, f, directory / f'gemm2_e4m3_K{k}_f{f}.ptx')
    for c, r, limit in ((64,4,0), (128,4,0), (256,3,80), (256,4,64)):
        for proj in (0, 1):
            yield ('ffn_e4m3', c, r, directory / f'ffn_e4m3_C{c}_R{r}{"_proj" if proj else ""}.ptx', limit, proj)
    for k, f, p, s in ((4096,1,32,4), (1024,0,16,2), (1024,1,8,4), (1024,0,8,4)):
        yield ('gemmt_e4m3', k, f, p, s, directory / f'gemmt_e4m3_K{k}_f{f}_p{p}_s{s}.ptx')
    for s, f in ((4,4), (2,8), (4,8)):
        yield ('reduce_e4m3', s, f, directory / f'reduce_e4m3_s{s}_f{f}.ptx')
    for k, f, s in ((1024,6,1), (4096,5,4), (1024,8,2), (1024,5,4)):
        yield ('gemmv_e4m3', k, f, s, directory / f'gemmv_e4m3_K{k}_f{f}_s{s}.ptx')
        yield ('gemmv_e4m3', k, f, s, directory / f'gemmv_e4m3_K{k}_f{f}_s{s}_m96.ptx', 4, 6, 0, 0, 0, 96)
    for p in (64,128,192,256):
        yield ('global_attention_e4m3', p, directory / f'global_attention_e4m3_p{p}.ptx')
    yield ('global_attention_stream_e4m3', 'normalize', directory / 'global_normalize_e4m3.ptx')
    yield ('global_attention_stream_e4m3', 'attention', directory / 'global_attention_stream_e4m3.ptx', 4)
    for k, f in ((512,4), (512,5), (512,13), (256,5), (4096,5), (1024,8), (1024,5)):
        yield ('gemmv_e4m3', k, f, 1, directory / f'gemmv_e4m3_K{k}_f{f}_s1_m96.ptx', 4, 6, 0, 0, 0, 96)
    for f in (2,66,74,130,48):
        yield ('block32_e4m3', f, directory / f'block32_e4m3_f{f}.ptx')
