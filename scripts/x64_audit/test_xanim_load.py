"""Exercise the production raw animation loader on x86 and x64."""
from pathlib import Path
import os
import re
import subprocess

root = Path.cwd()
out = Path(os.environ['TEMP'])/'kiwi-x64-epic/xanim-load'
out.mkdir(parents=True, exist_ok=True)
vc = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
version = '10.0.26100.0'
includes = [root/'src', root/'deps', root/'deps/msslib', root/'build/_deps/tracy-src/public', vc/'include']
includes += [sdk/'Include'/version/n for n in ['ucrt', 'shared', 'um']]
includes += [Path('C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/include')]


def extract(text,name):
    m=re.search(r'^\w[^\n;]*\b'+name+r'\s*\([^;]*?\)\s*\n\{',text,re.M)
    return text[m.start():text.index('\n}',m.end())+2]+'\n'
source=r'''
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <universal/com_memory.h>
#include <xanim/xanim.h>
#include <universal/com_files.h>
#include <Windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <string>
struct Block{byte *p;int size;bool temp;};
static std::vector<Block> blocks;
static std::vector<byte> fileBytes,wire;
static std::vector<std::string> strings;
static void checkBlocks(){for(const Block &b:blocks){for(int i=0;i<32;++i){assert(b.p[b.size+i]==0xCD);}}}
static void *allocate(int n,bool temporary){assert(n>0);byte *p=(byte*)_aligned_malloc(n+32,32);assert(p);memset(p,0xA5,n);memset(p+n,0xCD,32);blocks.push_back({p,n,temporary});return p;}
void *Alloc(int n){return allocate(n,false);}
void MyAssertHandler(const char *file,int line,int,const char *fmt,...){fprintf(stderr,"%s:%d ",file,line);va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);abort();}
void Com_Error(errorParm_t,const char *fmt,...){fprintf(stderr,"%s\n",fmt);throw 1;}
void Com_PrintError(int,const char*,...){}
int Com_sprintf(char *out,unsigned int size,const char *format,...){va_list ap;va_start(ap,format);int n=vsnprintf(out,size,format,ap);va_end(ap);return n;}
int I_strnicmp(const char *a,const char *b,int n){return _strnicmp(a,b,n);}
int FS_ReadFile(const char*,void **out){*out=malloc(fileBytes.size());memcpy(*out,fileBytes.data(),fileBytes.size());return (int)fileBytes.size();}
void FS_FreeFile(char *p){free(p);}
uint SL_GetStringOfSize(const char *s,uint,uint,mtType_t){for(size_t i=0;i<strings.size();++i){if(strings[i]==s){return (uint)i+1;}}strings.push_back(s);return (uint)strings.size();}
uint SL_GetString_(const char *s,uint user,mtType_t type){return SL_GetStringOfSize(s,user,(uint)strlen(s)+1,type);}
void Vec3Scale(const float *in,float scale,float *out){for(int i=0;i<3;++i){out[i]=in[i]*scale;}}
HunkUser *Hunk_UserCreate(int,const char*,bool,bool,int){return (HunkUser*)malloc(sizeof(HunkUser));}
void *Hunk_UserAlloc(HunkUser*,uint n,int align){assert(align>=alignof(void*));return allocate(n,true);}
void Hunk_UserDestroy(HunkUser *u){checkBlocks();for(size_t i=blocks.size();i-->0;){if(blocks[i].temp){_aligned_free(blocks[i].p);blocks.erase(blocks.begin()+i);}}free(u);}
'''
text=(root/'src/xanim/xanim_load_obj.cpp').read_text()
source+=text[text.index('enum $'):text.index('XModelPieces *__cdecl XModelPiecesLoadFile')]
for name in ['LoadTrans','ConsumeQuat','ConsumeQuat2','GetDeltaQuaternions','GetDeltaTranslations','GetQuaternions','GetTranslations','ReadNoteTracks','XAnimLoadFile']:
    source+=extract(text,name)
source+=r'''
template<class T> void put(T value){const byte *p=(const byte*)&value;fileBytes.insert(fileBytes.end(),p,p+sizeof(T));}
void putString(const char *s){while(*s){put<byte>(*s++);}put<byte>(0);}
void indices(int count,int frames){if(count<frames){for(int k=0;k<count;++k){int index=k*(frames-1)/(count-1);if(frames<=256){put<byte>((byte)index);}else{put<uint16_t>((uint16_t)index);}}}}
void quat(int type,int frames,int keys)
{
    int count=type==0?0:(type>=3?1:keys);put<uint16_t>((uint16_t)count);if(count>1){indices(count,frames);}
    for(int k=0;k<count;++k){put<int16_t>((int16_t)(k*5));if(type==2||type==4){put<int16_t>(12);put<int16_t>(23);}}
}
void trans(int type,int frames,int keys)
{
    int count=type==3?0:(type==2?1:keys);put<uint16_t>((uint16_t)count);
    if(!count){return;}if(count==1){put<float>(1);put<float>(2);put<float>(3);return;}
    indices(count,frames);put<byte>(type==0);for(int i=0;i<3;++i){put<float>((float)i);}for(int i=0;i<3;++i){put<float>((float)(i+10));}
    for(int k=0;k<count*3;++k){if(type==0){put<byte>((byte)k);}else{put<uint16_t>((uint16_t)(k*17));}}
}
void append(const void *p,size_t n){if(n){const byte *b=(const byte*)p;wire.insert(wire.end(),b,b+n);}}
int main()
{
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    for(int scenario=0;scenario<3;++scenario)
    {
        int frames=scenario==0?6:300,keys=scenario==2?67:3;
        fileBytes.clear();strings.clear();
        put<uint16_t>(17);put<uint16_t>((uint16_t)frames);put<int16_t>(10);put<byte>(2);put<byte>(0);put<int16_t>(30);
        quat(1,frames,keys);trans(0,frames,keys);
        put<byte>(0);put<byte>(0);
        byte simple[2]={};for(int i=0;i<10;++i){if(i%5!=2&&i%5!=4){simple[i/8]|=1u<<(i%8);}}
        put<byte>(simple[0]);put<byte>(simple[1]);
        for(int i=0;i<10;++i){char b[8];sprintf(b,"bone%d",i);putString(b);}
        for(int i=0;i<10;++i){quat(i%5,frames,keys);trans(i%4,frames,keys);}
        put<byte>(1);putString("mid");put<uint16_t>((uint16_t)(frames/2));
        XAnimParts *parts=XAnimLoadFile((char*)"fixture",Alloc);assert(parts&&parts->boneCount[9]==10);
        for(int i=0;i<5;++i){assert(parts->boneCount[i]==2);}assert(parts->boneCount[5]==3&&parts->boneCount[6]==3&&parts->boneCount[7]==2&&parts->boneCount[8]==2);
        assert(parts->notifyCount==2&&fabs(parts->notify[0].time-(float)(frames/2)/(frames-1))<0.00001f);
        assert(parts->deltaPart&&parts->deltaPart->quat->size==keys-1&&parts->deltaPart->trans->size==keys-1);
        append(parts->boneCount,sizeof(parts->boneCount));append(parts->names,10*sizeof(uint16_t));
        append(parts->dataByte,parts->dataByteCount);append(parts->dataShort,parts->dataShortCount*sizeof(int16_t));append(parts->dataInt,parts->dataIntCount*sizeof(int));
        append(parts->randomDataByte,parts->randomDataByteCount);append(parts->randomDataShort,parts->randomDataShortCount*sizeof(int16_t));
        append(parts->indices.data,parts->indexCount*(frames<=256?1:sizeof(uint16_t)));append(parts->notify,parts->notifyCount*sizeof(XAnimNotifyInfo));
        checkBlocks();for(const Block &b:blocks){_aligned_free(b.p);}blocks.clear();
    }
    FILE *out=fopen("packed.bin","wb");assert(out);fwrite(wire.data(),1,wire.size(),out);fclose(out);
    puts("PASS: production raw XAnim loader, all quaternion/translation categories, delta tracks, narrow/wide and long index tables, fractional notifications, poisoned allocations and canaries");
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
    result = subprocess.run([str(directory/'regression.exe')],capture_output=True,text=True,cwd=directory)
    (directory/'run.txt').write_text(result.stdout+result.stderr)
    assert result.returncode==0,result.stdout+result.stderr
    print(arch,result.stdout.strip())
assert (out/'x86/packed.bin').read_bytes()==(out/'x64/packed.bin').read_bytes()
print('PASS: packed animation bytes match across architectures')
