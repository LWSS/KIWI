"""Cross-architecture Huffman and SP message regression using production code."""
from pathlib import Path
import os
import re
import subprocess

root = Path.cwd()
out = Path(os.environ['TEMP'])/'kiwi-x64-epic/qcommon'
out.mkdir(parents=True, exist_ok=True)
vc = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
version = '10.0.26100.0'
includes = [root/'src', root/'deps', root/'deps/msslib', root/'build/_deps/tracy-src/public', vc/'include']
includes += [sdk/'Include'/version/n for n in ['ucrt', 'shared', 'um']]
includes += [Path('C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/include')]
source = r'''
#include <universal/q_shared.h>
#include <qcommon/msg.h>
#include <qcommon/qcommon.h>
#include <qcommon/com_bsp.h>
#include <bgame/bg_public.h>
#include <bgame/bg_local.h>
#include <qcommon/huffman.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <qcommon/msg.cpp>
#undef static_assert
static_assert(sizeof(SpawnVar)==(sizeof(void*)==8?3088:2572));
static dvar_t shownet;
const dvar_t *cl_shownet=&shownet;
void Com_Memset(void *p,int v,size_t n){memset(p,v,n);}
void Com_Printf(int,const char *format,...){char text[1024];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);}
void MyAssertHandler(const char*,int,int,const char*,...){abort();}
void Com_Error(errorParm_t,const char*,...){abort();}
void track_static_alloc_internal(void*,int,const char*,int){}
static unsigned __int64 Identity64(unsigned __int64 value){return value;}
unsigned __int64(__cdecl *LittleLong64)(unsigned __int64)=Identity64;
static huffman_t huffman;
clipMap_t cm;
static uint32_t diskNodes[2][9]={{0,1,0xffffffffu},{1,0xfffffffeu,0xffffffffu}};
static byte *allocated;
static uint allocatedSize;
char *Com_GetBspLump(LumpType type,uint size,uint *count)
{
    assert(type==LUMP_NODES&&size==36);*count=2;return (char*)diskNodes;
}
uint8_t *CM_Hunk_Alloc(uint size,const char*,int)
{
    allocated=(byte*)malloc(size+16);allocatedSize=size;memset(allocated,0xa5,size+16);return allocated;
}
static __declspec(align(16)) byte tempNodes[4096];
static uint tempUsed;
char *TempMalloc(uint size){char *p=(char*)tempNodes+tempUsed;tempUsed+=size;assert(tempUsed<=sizeof(tempNodes));return p;}
// EXTRACT_COLLISION_FUNCTIONS
int main(int argc,char **argv)
{
    setbuf(stdout,NULL);
    assert(argc==2);
    FILE *gold=fopen(argv[1],"wb");assert(gold);
    cplane_s planes[2]={};cm.planes=planes;CMod_LoadNodes();
    assert(allocatedSize==2*sizeof(cNode_t)&&cm.numNodes==2);
    assert(cm.nodes[0].plane==planes&&cm.nodes[1].plane==planes+1);
    assert(cm.nodes[0].children[1]==-1&&cm.nodes[1].children[0]==-2);
    for(uint i=0;i<16;i++) { assert(allocated[allocatedSize+i]==0xa5); }
    free(allocated);
    TempMalloc(sizeof(cLeafBrushNode_s));
    for(int i=1;i<100;i++)
    {
        cLeafBrushNode_s *node=CMod_AllocLeafBrushNode();
        assert((byte*)node==tempNodes+i*sizeof(cLeafBrushNode_s));
        assert(((uintptr_t)node%alignof(cLeafBrushNode_s))==0);
    }
    if(sizeof(void*)==8) { assert((uintptr_t)&huffman>0xffffffffu); }
    for(int pattern=0;pattern<3;pattern++)
    {
        Huff_Init(&huffman);int frequencies[256];
        for(int i=0;i<256;i++) { frequencies[i]=pattern==0?1:pattern==1?i+1:(i*37)%91+1; }
        Huff_BuildFromData(&huffman.compressDecompress,frequencies);
        uint8_t data[8192]={};int offset=0;
        for(int i=0;i<1024;i++) { Huff_offsetTransmit(&huffman.compressDecompress,i%256,data,&offset); }
        fwrite(&offset,sizeof(int),1,gold);fwrite(data,1,(offset+7)/8,gold);
        int read=0;
        for(int i=0;i<1024;i++)
        {
            int ch=-1;assert(Huff_offsetReceive(huffman.compressDecompress.tree,&ch,data,&read,offset));
            assert(ch==i%256);
        }
        assert(read==offset);
    }
    puts("huffman passed");
    byte intBuffer[16]={};msg_t intMsg;
    MSG_Init(&intMsg,intBuffer,sizeof(intBuffer));
    MSG_WriteInt64(&intMsg,0xfedcba9876543210ULL);MSG_BeginReading(&intMsg);
    assert(MSG_ReadInt64(&intMsg)==0xfedcba9876543210ULL);
    assert(MSG_ReadInt64(&intMsg)==0&&intMsg.overflowed);
    shownet.current.integer=4;
    playerState_s input={},decoded={};
    for(int i=0;i<143;i++)
    {
        const netField_t *f=&playerStateFields[i];
        if(f->bits) { *(int*)((byte*)&input+f->offset)=1; }
        else { *(float*)((byte*)&input+f->offset)=.25f; }
    }
    byte buffer[65536]={};msg_t msg;
    MSG_Init(&msg,buffer,sizeof(buffer));MSG_WriteDeltaPlayerstate(&msg,&input);
    assert(!msg.overflowed);fwrite(buffer,1,msg.cursize,gold);
    MSG_BeginReading(&msg);MSG_ReadDeltaPlayerstate(&msg,&decoded);assert(!msg.overflowed);
    for(int i=0;i<143;i++)
    {
        int offset=playerStateFields[i].offset;
        assert(memcmp((byte*)&input+offset,(byte*)&decoded+offset,sizeof(int))==0);
    }
    puts("playerstate passed");
    hudelem_s hud[256]={},hudOut[256]={};
    hud[0].type=HE_TYPE_TEXT;hud[0].value=1.25f;hud[0].x=5.5f;hud[0].y=-3.25f;
    MSG_Init(&msg,buffer,sizeof(buffer));MSG_WriteDeltaHudElems(&msg,hud,256);
    assert(!msg.overflowed);fwrite(buffer,1,msg.cursize,gold);
    MSG_BeginReading(&msg);MSG_ReadDeltaHudElems(&msg,hudOut,256);assert(!msg.overflowed);
    assert(memcmp(hud,hudOut,sizeof(hud))==0);
    fclose(gold);puts("PASS: Huffman symbols, SP playerstate fields and HUD roundtrips");
}
'''
test = out/'regression.cpp'
collision = (root/'src/qcommon/cm_load_obj.cpp').read_text()
functions = ''
for name in ['CMod_LoadNodes', 'CMod_AllocLeafBrushNode']:
    match = re.search(r'^\w[^\n;]*\b'+name+r'\s*\(', collision, re.M)
    functions += collision[match.start():collision.index('\n}', match.end())+2]+'\n'
source = source.replace('// EXTRACT_COLLISION_FUNCTIONS', functions)
test.write_text(source)
probe = out/'regression-diagnostic.h'
probe.write_text('#define static_assert(...)\n')
for arch in ['x86', 'x64']:
    directory = out/arch
    directory.mkdir(exist_ok=True)
    flags = ['/nologo', '/O2', '/Gy', '/std:c++20', '/EHsc', '/DWIN32', '/DKISAK_SP',
             '/D_ALLOW_KEYWORD_MACROS', '/DCPUSTRING="audit"', '/FI'+str(probe)]
    flags += ['/I'+str(p) for p in includes]
    files = [root/'src/qcommon/huffman.cpp', test]
    objects = []
    for file in files:
        obj = directory/(file.stem+'.obj')
        result = subprocess.run([str(vc/f'bin/Hostx64/{arch}/cl.exe')]+flags+['/c', str(file), '/Fo'+str(obj)], capture_output=True, text=True)
        (directory/(file.stem+'.log')).write_text(result.stdout+result.stderr)
        assert result.returncode==0, result.stdout+result.stderr
        objects.append(str(obj))
    command = [str(vc/'bin/Hostx64/x64/link.exe'), '/nologo', '/OPT:REF', '/OUT:'+str(directory/'regression.exe')]+objects
    command += ['/LIBPATH:'+str(vc/f'lib/{arch}')]
    command += ['/LIBPATH:'+str(sdk/'Lib'/version/n/arch) for n in ['ucrt', 'um']]
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode==0, result.stdout+result.stderr
    result = subprocess.run([str(directory/'regression.exe'), str(directory/'wire.bin')], capture_output=True, text=True)
    (directory/'run.txt').write_text(result.stdout+result.stderr)
    assert result.returncode==0, result.stdout+result.stderr
    print(arch,result.stdout.strip())
assert (out/'x86/wire.bin').read_bytes()==(out/'x64/wire.bin').read_bytes()
print('PASS: byte-identical x86/x64 wire output')
