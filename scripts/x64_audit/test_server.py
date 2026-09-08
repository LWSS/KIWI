"""Exercise production server initialization and native pointer traversal."""
from pathlib import Path
import os
import re
import subprocess

root = Path.cwd()
out = Path(os.environ['TEMP'])/'kiwi-x64-epic/server'
out.mkdir(parents=True, exist_ok=True)
vc = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
version = '10.0.26100.0'
includes = [root/'src', root/'deps', root/'deps/msslib', root/'build/_deps/tracy-src/public', vc/'include']
includes += [sdk/'Include'/version/n for n in ['ucrt', 'shared', 'um']]
includes += [Path('C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/include')]

def extract(file, name):
    text = (root/'src/server'/file).read_text()
    match = re.search(r'^\w[^\n;]*\b'+name+r'\s*\(', text, re.M)
    return text[match.start():text.index('\n}', match.end())+2]+'\n'

source = r'''
#include <universal/q_shared.h>
#include <server/server.h>
#include <universal/com_files.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#undef static_assert
server_t sv;
unsigned char g_buf[2][3145728];
int g_bufSize[2];
server_demo_history_t g_historyBuffers[2];
void MyAssertHandler(const char*,int,int,const char*,...){}
void Com_PrintError(int,const char*,...){}
int FS_FileSeek(FILE *f,long offset,int origin){return fseek(f,offset,origin);}
unsigned int FS_FileWrite(const void *p,unsigned int n,FILE *f){return (unsigned int)fwrite(p,1,n,f);}
unsigned int FS_FileRead(void *p,unsigned int n,FILE *f){return (unsigned int)fread(p,1,n,f);}
'''
demo=(root/'src/server/sv_demo.cpp').read_text()
source+=demo[demo.index('struct ServerDemoHistoryRecord'):demo.index('bool __cdecl SV_WriteHistory')]
for file,names in {
 'sv_demo.cpp':['SV_GetHistoryIndex','SV_GetBufferIndex','SV_HistoryAlloc','SV_HistoryFree','SV_WriteHistory','SV_ReadHistory'],
 'sv_snapshot.cpp':['SV_AddEntToSnapshot'],
}.items():
    for name in names:
        source+=extract(file,name)
source+=r'''
int main()
{
    assert(offsetof(server_demo_history_t,save)==72);
    if(sizeof(void*)==4){
        assert(sizeof(server_demo_history_t)==172);
        printf("x86 message offsets: %zu %zu\n",offsetof(server_t,demo)+offsetof(server_demo_t,msg)+offsetof(msg_t,cursize),offsetof(server_t,demo)+offsetof(server_demo_t,msg)+offsetof(msg_t,readcount));
    }
    server_demo_history_t *h=&g_historyBuffers[0];
    if(sizeof(void*)==8){assert((uintptr_t)h>UINT32_MAX);}
    assert(SV_GetBufferIndex(g_buf[0])==0);
    assert(SV_GetBufferIndex(g_buf[1]+sizeof(g_buf[1])-1)==1);
    unsigned char *p=NULL;
    assert(!SV_HistoryAlloc(h,&p,-1)&&!p);
    assert(!SV_HistoryAlloc(h,&p,INT_MAX)&&!p);
    assert(SV_HistoryAlloc(h,&p,sizeof(g_buf[0])));
    unsigned char *saved=p;
    assert(!SV_HistoryAlloc(h,&p,1)&&p==saved);
    SV_HistoryFree(saved,sizeof(g_buf[0]));
    h->manual=true;h->time=123;h->randomSeed=321;h->msgReadcount=87;h->msgBit=12;
    strcpy(h->name,"native roundtrip");
    h->save.bufLen=7;h->cmBufLen=9;h->freeEntBufLen=11;
    assert(SV_HistoryAlloc(h,&h->save.buf,7));memset(h->save.buf,0xa1,7);
    assert(SV_HistoryAlloc(h,&h->cmBuf,9));memset(h->cmBuf,0xb2,9);
    assert(SV_HistoryAlloc(h,&h->freeEntBuf,11));memset(h->freeEntBuf,0xc3,11);
    FILE *f=tmpfile();assert(f);assert(SV_WriteHistory(f,h));assert(ftell(f)==199);
    rewind(f);
    server_demo_history_t *dst=&g_historyBuffers[1];
    memset(dst,0xee,sizeof(server_demo_history_t));
    assert(SV_ReadHistory(f,dst));fclose(f);
    assert(dst->manual&&dst->time==123&&dst->randomSeed==321&&dst->msgReadcount==87);
    assert(!strcmp(dst->name,h->name));
    assert(dst->save.buf==g_buf[1]&&dst->cmBuf==g_buf[1]+7&&dst->freeEntBuf==g_buf[1]+16);
    for(int i=0;i<11;i++){assert(dst->freeEntBuf[i]==0xc3);}
    memset(sv.cmd,0xa5,sizeof(sv.cmd));
    for(int i=0;i<2048;i++){SV_AddEntToSnapshot(i);}
    SV_AddEntToSnapshot(2048);
    assert(sv.entityNumbers.numSnapshotEntities==2048);
    for(int i=0;i<2048;i++){assert(sv.entityNumbers.snapshotEntities[i]==i);}
    assert((unsigned char)sv.cmd[0]==0xa5);
    puts("PASS: history capacity/overflow, native buffer addresses, 199-byte cache roundtrip, snapshot capacity");
}
'''
test = out/'regression.cpp'
test.write_text(source)
probe = out/'regression-diagnostic.h'
probe.write_text('#define static_assert(...)\n')
for arch in ['x86','x64']:
    directory = out/arch
    directory.mkdir(exist_ok=True)
    args = ['/nologo','/O2','/std:c++20','/EHsc','/DRELEASE_ASSERTS','/DWIN32','/DKISAK_SP',
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
