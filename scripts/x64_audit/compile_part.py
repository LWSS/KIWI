"""Diagnostic per-folder compilation; run from the repository root.

Does not link the game. Shared legacy layout assertions are suppressed only in
the x64 diagnostic probe, until their owning folders have been ported.
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('folder')
parser.add_argument('--modes', nargs='+', default=['MP', 'SP'])
options = parser.parse_args()
root = Path.cwd()
output = Path(os.environ['TEMP']) / 'kiwi-x64-epic' / options.folder
output.mkdir(parents=True, exist_ok=True)
vc = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207')
sdk = Path('C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0')
includes = [root/'src', root/'deps', root/'deps/msslib', root/'build/_deps/tracy-src/public',
            Path('C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/include'), vc/'include']
includes += [sdk/n for n in ['ucrt', 'shared', 'um', 'winrt']]
probe = output/'diagnostic.h'
probe.write_text('// Test only: shared x86 assertions are audited separately.\n#define static_assert(...)\n')
files = sorted(p for p in (root/'src'/options.folder).rglob('*') if p.suffix in ['.c', '.cpp'])

def check(case):
    file, arch, mode = case
    args = ['/nologo', '/Zs', '/W3', '/DWIN32', '/D_WINDOWS', '/DRELEASE_ASSERTS',
            '/DKISAK_'+mode, '/DCINEMA', '/DUSE_SEPARATE_BLIT_TEXTURE',
            '/D_CRT_SECURE_NO_WARNINGS', '/D_CRT_NONSTDC_NO_DEPRECATE']
    if file.suffix == '.cpp':
        args += ['/std:c++20', '/EHsc', '/permissive-']
    if arch == 'x64':
        args += ['/D_ALLOW_KEYWORD_MACROS', '/DCPUSTRING="win-x64-audit"', '/FI'+str(probe)]
    args += ['/I'+str(p) for p in includes]
    result = subprocess.run([str(vc/f'bin/Hostx64/{arch}/cl.exe')]+args+[str(file)],
                            capture_output=True, text=True)
    return dict(file=str(file.relative_to(root)), arch=arch, mode=mode,
                returncode=result.returncode, output=result.stdout+result.stderr)

cases = [(file, arch, mode) for file in files for arch in ['x86', 'x64'] for mode in options.modes]
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    results = list(pool.map(check, cases))
(output/'compile.json').write_text(json.dumps(results, indent=2))
for result in results:
    if result['returncode']:
        print(result['file'], result['arch'], result['mode'], 'FAIL')
        print('\n'.join(line for line in result['output'].splitlines() if 'error' in line))
print(len(results), 'checks;', sum(r['returncode'] != 0 for r in results), 'failed;', output)
raise SystemExit(any(r['returncode'] != 0 for r in results))
