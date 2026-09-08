"""Exercise production universal initialization and native pointer traversal."""
from pathlib import Path
import os
import re
import subprocess

root = Path.cwd()
out = Path(os.environ['TEMP'])/'kiwi-x64-epic/universal'
out.mkdir(parents=True, exist_ok=True)
vc = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
version = '10.0.26100.0'
includes = [root/'src', root/'deps', root/'deps/msslib', root/'build/_deps/tracy-src/public', vc/'include']
includes += [sdk/'Include'/version/n for n in ['ucrt', 'shared', 'um']]
includes += [Path('C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/include')]

def extract(file, name):
    text = (root/'src/universal'/file).read_text()
    match = re.search(r'^\w[^\n;]*\b'+name+r'\s*\(', text, re.M)
    return text[match.start():text.index('\n}', match.end())+2]+'\n'

source=r'''
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <universal/com_memory.h>
#include <universal/pool_allocator.h>
#include <universal/memfile.h>
#include <universal/q_parse.h>
#include <universal/physicalmemory.h>
#include <qcommon/threads.h>
#include <Windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#undef static_assert
static_assert(sizeof(parseInfo_t)==(sizeof(void*)==8?0x438:0x420));
static_assert(sizeof(ParseThreadInfo)==(sizeof(void*)==8?0x4798:0x460C));
static_assert(sizeof(com_parse_mark_t)==(sizeof(void*)==8?32:20));
static_assert(offsetof(HunkUser,buf)%32==0);
void MyAssertHandler(const char *file,int line,int,const char *fmt,...){fprintf(stderr,"%s:%d ",file,line);va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);abort();}
void Com_Error(errorParm_t,const char*,...){throw 1;}
void *Z_VirtualReserve(int n){void *p=VirtualAlloc(NULL,n,MEM_RESERVE,PAGE_READWRITE);assert(p);return p;}
void Z_VirtualCommit(void *p,int n){assert(n>0&&VirtualAlloc(p,n,MEM_COMMIT,PAGE_READWRITE));}
void Z_VirtualDecommit(void *p,int n){assert(n>0&&VirtualFree(p,n,MEM_DECOMMIT));}
void Z_VirtualFree(void *p){assert(VirtualFree(p,0,MEM_RELEASE));}
void *Z_Malloc(int n,const char*,int){return malloc(n);}
void Z_Free(void *p,int){free(p);}
bool Sys_IsMainThread(){return true;}
bool Sys_IsRenderThread(){return false;}
bool Sys_IsDatabaseThread(){return false;}
bool Sys_IsServerThread(){return false;}
void Com_Printf(int,const char*,...){}
void Com_PrintError(int,const char*,...){}
char *va(const char *fmt,...){static char b[4096];va_list ap;va_start(ap,fmt);vsnprintf(b,sizeof(b),fmt,ap);va_end(ap);return b;}
#include <universal/q_parse.cpp>
void track_PrintAllInfo(){}
void track_hunk_alloc(int,int,const char*,int){}
void track_hunk_allocLow(int,int,const char*,int){}
void track_temp_alloc(int,int,int,const char*){}
void track_temp_free(int,int,const char*){}
void Hunk_CheckTempMemoryClear(){}
void Hunk_CheckTempMemoryHighClear(){}
const char *CopyString(const char *s){return _strdup(s);}
void FreeString(const char *s){free((void*)s);}
'''
mem=(root/'src/universal/com_memory.cpp').read_text()
for decl in ['struct hunkUsed_t','struct alignas(16) hunkHeader_t']:
    a=mem.index(decl);source+=mem[a:mem.index('};',a)+2]+'\n'
source+='hunkUsed_t hunk_low={},hunk_high={};byte *s_hunkData;int s_hunkTotal;\n'
for name in ['Hunk_UserCreate','Hunk_UserAlloc','Hunk_UserSetPos','Hunk_UserDestroy','Hunk_UserReset','Hunk_AllocateTempMemory','Hunk_FreeTempMemory','Hunk_AllocAlign','Hunk_AllocLowAlign']:
    source+=extract('com_memory.cpp',name)
for name in ['Pool_Init','Pool_Alloc','Pool_Free','Pool_FreeCount']:
    source+=extract('pool_allocator.cpp',name)
for name in ['MemFile_GetSegmentAddess','MemFile_CopySegments']:
    source+=extract('memfile.cpp',name)
for name in ['Dvar_ShouldFreeCurrentString','Dvar_ShouldFreeLatchedString','Dvar_ShouldFreeResetString','Dvar_FreeString','Dvar_CopyString','Dvar_WeakCopyString','Dvar_AssignResetStringValue','Dvar_AssignCurrentStringValue','Dvar_AssignLatchedStringValue','Dvar_UpdateResetValue','Dvar_UpdateValue','Dvar_SetLatchedValue','Dvar_EnumToString']:
    source+=extract('dvar.cpp',name)
source+=extract('com_shared.cpp','Com_Memset')
source+='alignas(16) static byte g_largeLocalBuf[0x80000];int g_largeLocalPos;\n'
for name in ['LargeLocalRoundSize','LargeLocalBegin','LargeLocalEnd','LargeLocalGetBuf']:
    source+=extract('com_memory.cpp',name)
for name in ['PMem_BeginAllocInPrim','PMem_EndAllocInPrim','PMem_FreeIndex']:
    source+=extract('physicalmemory.cpp',name)
source+=r'''
int main()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    struct PoolItem{freenode node;void *value;};
    PoolItem items[160];pooldata_t pool;
    Pool_Init((char*)items,&pool,sizeof(PoolItem),160);assert(Pool_FreeCount(&pool)==160);
    for(int i=0;i<160;++i){assert(Pool_Alloc(&pool)==&items[i].node);}
    assert(!Pool_Alloc(&pool));Pool_Free(&items[159].node,&pool);assert(Pool_Alloc(&pool)==&items[159].node);
    Pool_Init((char*)items,&pool,sizeof(PoolItem),0);assert(!Pool_Alloc(&pool));
    HunkUser *user=Hunk_UserCreate(65536,"test",false,false,0);
    uintptr_t first=(uintptr_t)user->buf;
    for(int i=0;i<600;++i){void*p=Hunk_UserAlloc(user,101,32);assert((uintptr_t)p%32==0);memset(p,i,101);}
    assert(user->next);Hunk_UserReset(user);assert(user->pos==first&&!user->next&&user->current==user);
    for(int i=0;i<4096-(int)offsetof(HunkUser,buf);++i){assert(user->buf[i]==0);}
    Hunk_UserDestroy(user);
    user=Hunk_UserCreate(4096,"fixed",true,false,0);
    Hunk_UserAlloc(user,4096-(uint)offsetof(HunkUser,buf),1);bool rejected=false;
    try{Hunk_UserAlloc(user,1,1);}catch(int){rejected=true;}assert(rejected);Hunk_UserDestroy(user);
    s_hunkTotal=1024*1024;s_hunkData=(byte*)Z_VirtualReserve(s_hunkTotal);
    unsigned int *temp=Hunk_AllocateTempMemory(23,"temp");assert((uintptr_t)temp%16==0);memset(temp,0xCC,23);
    hunkHeader_t *header=(hunkHeader_t*)((byte*)temp-sizeof(hunkHeader_t));
    assert(header->magic==0x89537892&&!strcmp(header->name,"temp"));Hunk_FreeTempMemory((char*)temp);assert(hunk_low.temp==0);
    byte *lo=Hunk_AllocLowAlign(123,32,"low",0),*hi=Hunk_AllocAlign(145,64,"high",0);
    assert((uintptr_t)lo%32==0&&(uintptr_t)hi%64==0&&lo<hi);
    for(int i=0;i<123;++i){assert(lo[i]==0);}for(int i=0;i<145;++i){assert(hi[i]==0);}
    Z_VirtualFree(s_hunkData);
    byte data[24]={};unsigned int length=8;memcpy(data,&length,4);memcpy(data+8,&length,4);memcpy(data+16,&length,4);
    MemoryFile file={};file.buffer=data;file.bufferSize=24;byte copy[24];
    assert(MemFile_CopySegments(&file,1,copy)==16&&!memcmp(copy,data+8,16));
    length=0xffffffff;memcpy(data,&length,4);rejected=false;try{MemFile_CopySegments(&file,1,copy);}catch(int){rejected=true;}assert(rejected);
    dvar_s var={};var.name="test";var.type=DVAR_TYPE_STRING;
    DvarValue value={};value.string="one";Dvar_UpdateResetValue(&var,value);Dvar_UpdateValue(&var,value);
    assert(!strcmp(var.current.string,"one")&&var.current.string==var.reset.string&&var.latched.string==var.current.string);
    value.string="two";Dvar_SetLatchedValue(&var,value);assert(!strcmp(var.latched.string,"two")&&var.current.string==var.reset.string);
    Dvar_UpdateValue(&var,var.latched);assert(!strcmp(var.current.string,"two")&&var.latched.string==var.current.string);
    value.string="three";Dvar_UpdateResetValue(&var,value);assert(!strcmp(var.reset.string,"three"));
    if(Dvar_ShouldFreeCurrentString(&var)){Dvar_FreeString(&var.current);}var.current.string=NULL;
    if(Dvar_ShouldFreeLatchedString(&var)){Dvar_FreeString(&var.latched);}var.latched.string=NULL;
    if(Dvar_ShouldFreeResetString(&var)){Dvar_FreeString(&var.reset);}var.reset.string=NULL;
    const char *options[3]={"first","second",NULL};var.type=DVAR_TYPE_ENUM;var.domain.enumeration.strings=options;var.domain.enumeration.stringCount=2;var.current.integer=1;
    assert(Dvar_EnumToString(&var)==options[1]);
    int a=LargeLocalBegin(1),b=LargeLocalBegin(33);assert(a==0&&b==16&&(uintptr_t)LargeLocalGetBuf(b)%16==0);LargeLocalEnd(b);LargeLocalEnd(a);
    rejected=false;try{LargeLocalBegin(0x80001);}catch(int){rejected=true;}assert(rejected&&g_largeLocalPos==0);
    PhysicalMemoryPrim prim={};const char *names[3]={"first","second","third"};
    for(int i=0;i<3;++i){prim.pos=i*64;PMem_BeginAllocInPrim(&prim,names[i]);prim.pos+=64;PMem_EndAllocInPrim(&prim,names[i]);}
    for(int i=2;i>=0;--i){PMem_FreeIndex(&prim,i);assert(prim.allocListCount==i&&prim.pos==i*64);}
    Com_InitParse();Com_BeginParseSession("regression");
    const char *text="alpha /* comment */ \"two words\"\n beta";com_parse_mark_t mark;
    assert(!strcmp(Com_Parse(&text)->token,"alpha"));Com_ParseSetMark(&text,&mark);
    assert(!strcmp(Com_Parse(&text)->token,"two words"));Com_ParseReturnToMark(&text,&mark);
    assert(!strcmp(Com_Parse(&text)->token,"two words"));assert(!strcmp(Com_Parse(&text)->token,"beta"));
    Com_EndParseSession();
    for(int i=0;i<15;++i){Com_BeginParseSession(names[i%3]);}
    rejected=false;try{Com_BeginParseSession("overflow");}catch(int){rejected=true;}assert(rejected);
    for(int i=0;i<15;++i){Com_EndParseSession();}
    char fill[65];Com_Memset(fill,0x1AB,sizeof(fill));for(int i=0;i<65;++i){assert((unsigned char)fill[i]==0xAB);}
    puts("PASS: native pool, committed hunk alignment/growth/reset/exhaustion, temp header roundtrip, memfile segments, dvar string ownership and enums, byte fill");
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
    assert result.returncode==0,str(result.returncode)+result.stdout+result.stderr
    result = subprocess.run([str(directory/'regression.exe')],capture_output=True,text=True)
    (directory/'run.txt').write_text(result.stdout+result.stderr)
    assert result.returncode==0,result.stdout+result.stderr
    print(arch,result.stdout.strip())
