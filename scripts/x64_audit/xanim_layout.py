"""Report native animation layouts for architecture-specific assertions."""
from pathlib import Path
import os
import re
import subprocess

root = Path.cwd()
out = Path(os.environ['TEMP'])/'kiwi-x64-epic/xanim-layout'
out.mkdir(parents=True, exist_ok=True)
vc = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
version = '10.0.26100.0'
includes = [root/'src', root/'deps', root/'deps/msslib', root/'build/_deps/tracy-src/public', vc/'include']
includes += [sdk/'Include'/version/n for n in ['ucrt', 'shared', 'um']]
includes += [Path('C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/include')]

headers = sorted((root/'src/xanim').glob('*.h'))
types = []
for header in headers:
    types += re.findall(r'static_assert\(sizeof\((\w+)\)',header.read_text())
source = '#include <universal/q_shared.h>\n#include <stdio.h>\n'
for header in headers:
    if header.name not in []:
        source += '#include <xanim/'+header.name+'>\n'
source += 'int main(){\n'
types += ['DObj_s','DSkel','DObjModel_s','XAnimEntry','XAnim_s','XAnimTree_s','XAnimInfo','XAnimDeltaPart','XAnimPartTrans','XAnimPartQuat','XAnimDeltaPartQuat','XAnimPartQuatPtr','XAnimPartTransPtr','XModelCollSurf_s','XSurfaceCollisionTree','PhysGeomList','PhysGeomInfo','BrushWrapper']
source = '#include <universal/q_shared.h>\n#include <physics/phys_local.h>\n'+source
for name in dict.fromkeys(types):
    source += 'printf("'+name+' %zu\\n",sizeof('+name+'));\n'
source += '}\n'
test = out/'regression.cpp'
test.write_text(source)
probe = out/'regression-diagnostic.h'
probe.write_text('#define static_assert(...)\n')
for arch in ['x86','x64']:
    directory = out/arch
    directory.mkdir(exist_ok=True)
    args = ['/nologo','/O2','/std:c++20','/EHsc','/DWIN32','/DKISAK_SP',
            '/D_ALLOW_KEYWORD_MACROS','/DCPUSTRING="audit"','/FI'+str(probe)]
    args += ['/I'+str(p) for p in includes]
    args += ['/Fe'+str(directory/'regression.exe'),'/Fo'+str(directory/'regression.obj'),str(test),'/link']
    args += ['/LIBPATH:'+str(vc/f'lib/{arch}')]
    args += ['/LIBPATH:'+str(sdk/'Lib'/version/n/arch) for n in ['ucrt','um']]
    result = subprocess.run([str(vc/f'bin/Hostx64/{arch}/cl.exe')]+args,capture_output=True,text=True)
    (directory/'build.txt').write_text(result.stdout+result.stderr)
    assert result.returncode==0,result.stdout+result.stderr
    result = subprocess.run([str(directory/'regression.exe')],capture_output=True,text=True)
    (directory/'run.txt').write_text(result.stdout+result.stderr)
    assert result.returncode==0,result.stdout+result.stderr
    print(arch,result.stdout.strip())
