"""Local toolchain recovery without downloads or extracting the real LLVM tree."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / 'scripts/linux/local-toolchain.sh'
PACKAGES = ['clang-18','libclang-cpp18','libllvm18','libclang-common-18-dev','libclang1-18',
            'llvm-18-linker-tools','libc++-18-dev','libc++1-18','libc++abi-18-dev',
            'libc++abi1-18','libunwind-18-dev','libunwind-18','libclang-rt-18-dev']

class ToolchainTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='NR toolchain space ')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.deps = self.root / 'deps'
        (self.deps/'downloads').mkdir(parents=True)
        self.bin = self.root/'bin'
        self.bin.mkdir()
        self.log = self.root/'extractions'
        self.env = dict(os.environ, PATH=str(self.bin)+':'+os.environ['PATH'],
                        NR_DEPS_DIR=str(self.deps), NR_BUILD_DIR=str(self.root/'build'),
                        TOOLCHAIN_LOG=str(self.log))
        self.env.pop('CXX',None)
        self.env.pop('CC',None)
        for package in PACKAGES:
            (self.deps/'downloads'/f'{package}_1%3a18.1.3-1ubuntu1_amd64.deb').write_text('cached')
        # The unrelated glibc package must never be extracted by the toolchain installer.
        (self.deps/'downloads/libc6-dev_2.39-0ubuntu8.9_amd64.deb').write_text('unrelated')
        self.script('apt-get', '#!/bin/bash\necho unexpected-download >&2; exit 90\n')
        self.script('dpkg-deb', '''#!/bin/bash
if [[ "$1" == -f ]]; then
  name=${2##*/}; echo "Package: ${name%%_*}"; echo "Version: ${FAKE_DEB_VERSION:-1:18.1.3-1ubuntu1}"; echo 'Architecture: amd64'; exit 0
fi
[[ "$1" == -x ]] || exit 91
echo "${2##*/}" >> "$TOOLCHAIN_LOG"
if [[ -n "${FAIL_PACKAGE:-}" && "$2" == *"$FAIL_PACKAGE"* ]]; then exit 37; fi
mkdir -p "$3/.fake-packages"
touch "$3/.fake-packages/${2##*/}"
# Emulate the installed payload only after all pinned packages have extracted.
if [[ $(find "$3/.fake-packages" -type f | wc -l) == 13 ]]; then
  "$FIXTURE_WRITER" "$3"
fi
''')
        self.writer = self.root/'payload.py'
        self.writer.write_text('''#!/usr/bin/env python3
import os, pathlib, sys
prefix = pathlib.Path(sys.argv[1])
files = ['usr/lib/llvm-18/lib/libLLVM.so.1','usr/lib/llvm-18/lib/libclang-cpp.so.18.1',
'usr/lib/llvm-18/lib/libclang-18.so.18','usr/lib/llvm-18/include/c++/v1/vector',
'usr/lib/llvm-18/lib/libc++.so','usr/lib/llvm-18/lib/libc++.a','usr/lib/llvm-18/lib/libc++abi.so',
'usr/lib/llvm-18/lib/libunwind.so','usr/lib/llvm-18/lib/clang/18/include/stddef.h',
'usr/lib/llvm-18/lib/clang/18/lib/linux/libclang_rt.builtins-x86_64.a',
'usr/lib/llvm-18/lib/LLVMgold.so']
for name in files:
    p=prefix/name; p.parent.mkdir(parents=True,exist_ok=True); p.write_text('payload')
compiler = '#!/bin/bash\\nif [[ "$1" == --version ]]; then echo "clang version 18.1.3"; exit; fi\\nif [[ -n "${FAIL_PROOF:-}" ]]; then exit 42; fi\\nwhile [[ $# -gt 0 ]]; do if [[ "$1" == -o ]]; then shift; printf "#!/bin/bash\\\\nexit 0\\\\n" > "$1"; chmod +x "$1"; fi; shift; done\\n'
for name in ['clang-18','clang++-18']:
    p=prefix/'usr/bin'/name; p.parent.mkdir(parents=True,exist_ok=True); p.write_text(compiler); p.chmod(0o755)
''')
        self.writer.chmod(0o755)
        self.env['FIXTURE_WRITER'] = str(self.writer)

    def script(self,name,body):
        path=self.bin/name;path.write_text(body);path.chmod(0o755)

    def run_helper(self,body,check=True,extra=None):
        result = subprocess.run(['bash','-c','set -e; source "$1"; '+body,'test',str(HELPER)],
                              env=dict(self.env,**(extra or {})),text=True,capture_output=True)
        if check and result.returncode:
            self.fail(result.stdout + result.stderr)
        return result

    def test_clang_only_prefix_is_not_selected(self):
        partial=self.deps/'llvm/usr/bin/clang++-18'
        partial.parent.mkdir(parents=True)
        partial.write_text('#!/bin/bash\nexit 0\n');partial.chmod(0o755)
        self.run_helper('! nr_toolchain_ready "$NR_DEPS_DIR/llvm"')
        # Exclude a compatible system compiler independently of the local-prefix gate.
        self.script('clang++','#!/bin/bash\nexit 1\n')
        result=subprocess.run(['bash','-c','source "$1"; printf "%s" "${CXX:-}"','test',str(ROOT/'scripts/linux/common.sh')],env=self.env,text=True,capture_output=True,check=True)
        self.assertEqual(result.stdout,'')

    def test_failure_then_retry_publishes_only_complete_pinned_payload(self):
        old=self.deps/'llvm';old.mkdir();(old/'old-prefix').write_text('keep until complete')
        failure=self.run_helper('nr_install_toolchain "$NR_DEPS_DIR"',check=False,extra={'FAIL_PACKAGE':'libllvm18_'})
        self.assertNotEqual(failure.returncode,0)
        self.assertTrue((old/'old-prefix').is_file())
        self.run_helper('! nr_toolchain_ready "$NR_DEPS_DIR/llvm"')
        self.run_helper('nr_install_toolchain "$NR_DEPS_DIR"; nr_toolchain_ready "$NR_DEPS_DIR/llvm"')
        self.assertFalse((old/'old-prefix').exists())
        extracted=self.log.read_text().splitlines()
        self.assertNotIn('libc6-dev_2.39-0ubuntu8.9_amd64.deb',extracted)
        self.assertEqual(extracted[-13:],[f'{p}_1%3a18.1.3-1ubuntu1_amd64.deb' for p in PACKAGES])
        before=self.log.read_text()
        self.run_helper('nr_install_toolchain "$NR_DEPS_DIR"')
        self.assertEqual(before,self.log.read_text())
        # A marker alone cannot conceal a subsequently missing runtime dependency.
        (old/'usr/lib/llvm-18/lib/libc++abi.so').unlink()
        self.run_helper('! nr_toolchain_ready "$NR_DEPS_DIR/llvm"')
        self.run_helper('nr_install_toolchain "$NR_DEPS_DIR"; nr_toolchain_ready "$NR_DEPS_DIR/llvm"')

    def test_failed_proof_never_publishes(self):
        old=self.deps/'llvm';old.mkdir();(old/'old-prefix').write_text('preserve')
        failure=self.run_helper('nr_install_toolchain "$NR_DEPS_DIR"',check=False,extra={'FAIL_PROOF':'1'})
        self.assertNotEqual(failure.returncode,0)
        self.assertTrue((old/'old-prefix').exists())
        self.run_helper('! nr_toolchain_ready "$NR_DEPS_DIR/llvm"')
        self.run_helper('nr_install_toolchain "$NR_DEPS_DIR"; nr_toolchain_ready "$NR_DEPS_DIR/llvm"')

    def test_incorrect_cached_package_version_is_not_extracted(self):
        failure=self.run_helper('nr_install_toolchain "$NR_DEPS_DIR"',check=False,extra={'FAKE_DEB_VERSION':'1:18.0.0'})
        self.assertNotEqual(failure.returncode,0)
        self.assertIn('Unexpected pinned toolchain package metadata',failure.stderr)
        self.assertFalse(self.log.exists())
        self.assertFalse((self.deps/'llvm').exists())

    def test_existing_complete_prefix_is_attested_without_extracting(self):
        subprocess.run([str(self.writer),str(self.deps/'llvm')],check=True)
        self.run_helper('nr_install_toolchain "$NR_DEPS_DIR"; nr_toolchain_ready "$NR_DEPS_DIR/llvm"')
        self.assertFalse(self.log.exists())

if __name__ == '__main__': unittest.main()
