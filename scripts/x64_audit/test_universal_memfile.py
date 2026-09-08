"""Exercise production universal initialization and native pointer traversal."""
from pathlib import Path
import os
import re
import subprocess

root = Path.cwd()
out = Path(os.environ['TEMP'])/'kiwi-x64-epic/universal-memfile'
out.mkdir(parents=True, exist_ok=True)
vc = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
version = '10.0.26100.0'
includes = [root/'src', root/'deps', root/'deps/msslib', root/'build/_deps/tracy-src/public', vc/'include']
includes += [sdk/'Include'/version/n for n in ['ucrt', 'shared', 'um']]
includes += [Path('C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/include')]

source=r'''
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <Windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
void MyAssertHandler(const char *file,int line,int,const char *fmt,...){fprintf(stderr,"%s:%d ",file,line);va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);abort();}
void Com_Error(errorParm_t,const char *fmt,...){fprintf(stderr,"%s\n",fmt);throw 1;}
void Com_Printf(int,const char*,...){}
char *va(const char *fmt,...){static char b[4096];va_list ap;va_start(ap,fmt);vsnprintf(b,sizeof(b),fmt,ap);va_end(ap);return b;}
bool Sys_IsMainThread(){return true;}
bool Sys_IsRenderThread(){return false;}
bool Sys_IsDatabaseThread(){return false;}
#include <universal/memfile.cpp>
int main()
{
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    static byte buffer[1048576],original[20000],decoded[20000];
    for(int compressed=0;compressed<2;++compressed)
    {
        MemoryFile f={};
        MemFile_InitForWriting(&f,sizeof(buffer),buffer,true,compressed!=0);
        for(int segment=0;segment<8;++segment)
        {
            if(segment){MemFile_StartSegment(&f,segment);}
            for(int i=0;i<20000;++i){original[i]=(i%129<65)?0:(byte)(i*37+segment);}
            for(int offset=0;offset<20000;){int n=(offset%97)+1;if(n>20000-offset){n=20000-offset;}MemFile_WriteData(&f,n,original+offset);offset+=n;}
            MemFile_WriteCString(&f,"native memfile");
        }
        MemFile_StartSegment(&f,-1);
        int used=MemFile_GetUsedSize(&f);assert(used>0&&used<sizeof(buffer));
        FILE *wire=fopen(compressed?"compressed.bin":"uncompressed.bin","wb");assert(wire);assert(fwrite(buffer,1,used,wire)==used);fclose(wire);
        MemFile_InitForReading(&f,used,buffer,compressed!=0);
        for(int segment=0;segment<8;++segment)
        {
            if(segment){MemFile_MoveToSegment(&f,segment);}
            for(int i=0;i<20000;++i){original[i]=(i%129<65)?0:(byte)(i*37+segment);}
            MemFile_ReadData(&f,sizeof(decoded),decoded);assert(!memcmp(original,decoded,sizeof(original)));
            assert(!strcmp(MemFile_ReadCString(&f),"native memfile"));
        }
        MemFile_MoveToSegment(&f,-1);
    }
    puts("PASS: actual zlib and uncompressed memfiles, eight segments, chunked zero/nonzero runs and strings");
}
'''
test = out/'regression.cpp'
test.write_text(source)
probe = out/'regression-diagnostic.h'
probe.write_text('#define static_assert(...)\n')
for arch in ['x86','x64']:
    directory = out/arch
    directory.mkdir(exist_ok=True)
    objects = []
    for path in sorted((root/'deps/zlib').glob('*.c')):
        if path.name in ['maketree.c', 'gzio.c']:
            continue
        obj = directory/(path.stem+'.obj')
        command = [str(vc/f'bin/Hostx64/{arch}/cl.exe'),'/nologo','/O2','/TC','/c','/Fo'+str(obj)]
        command += ['/I'+str(p) for p in includes]
        result = subprocess.run(command+[str(path)],capture_output=True,text=True)
        assert result.returncode==0,result.stdout+result.stderr
        objects.append(str(obj))
    args = ['/nologo','/O2','/std:c++20','/EHsc','/DRELEASE_ASSERTS','/DWIN32','/DKISAK_SP',
            '/D_ALLOW_KEYWORD_MACROS','/DCPUSTRING="audit"','/FI'+str(probe)]
    args += ['/I'+str(p) for p in includes]
    args += ['/Fe'+str(directory/'regression.exe'),'/Fo'+str(directory/'regression.obj'),str(test)]+objects+['/link']
    args += ['/LIBPATH:'+str(vc/f'lib/{arch}')]
    args += ['/LIBPATH:'+str(sdk/'Lib'/version/n/arch) for n in ['ucrt','um']]
    result = subprocess.run([str(vc/f'bin/Hostx64/{arch}/cl.exe')]+args,capture_output=True,text=True)
    (directory/'build.txt').write_text(result.stdout+result.stderr)
    assert result.returncode==0,str(result.returncode)+result.stdout+result.stderr
    result = subprocess.run([str(directory/'regression.exe')],capture_output=True,text=True,cwd=directory)
    (directory/'run.txt').write_text(result.stdout+result.stderr)
    assert result.returncode==0,result.stdout+result.stderr
    print(arch,result.stdout.strip())
for name in ['compressed.bin','uncompressed.bin']:
    assert (out/'x86'/name).read_bytes()==(out/'x64'/name).read_bytes(),name
print('PASS: cross-architecture memfile bytes match')

