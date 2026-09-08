"""Exercise the production raw animation loader on x86 and x64."""
from pathlib import Path
import os
import re
import subprocess

root = Path.cwd()
out = Path(os.environ['TEMP'])/'kiwi-x64-epic/xanim-runtime'
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
#include <xanim/dobj.h>
#include <xanim/xanim.h>
#include <xanim/xmodel.h>
#include <script/scr_memorytree.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <Windows.h>
#include <vector>
#define PROF_SCOPED(...)
struct Block{byte *p;int size;};
static std::vector<Block> blocks;
static int refs;
static bool g_anim_developer;
void MyAssertHandler(const char*,int,int,const char *fmt,...){fprintf(stderr,"%s\n",fmt);abort();}
void Com_Error(errorParm_t,const char *fmt,...){fprintf(stderr,"%s\n",fmt);throw 1;}
void Com_PrintWarning(int,const char*,...){}
void *Alloc(int n){assert(n>0);byte *p=(byte*)_aligned_malloc(n+32,32);assert(p);memset(p,0xA5,n);memset(p+n,0xCD,32);blocks.push_back({p,n});return p;}
void check(){for(const Block &b:blocks){for(int i=0;i<32;++i){assert(b.p[b.size+i]==0xCD);}}}
void Free(void *p,int n){check();for(size_t i=0;i<blocks.size();++i){if(blocks[i].p==p){assert(n==blocks[i].size);_aligned_free(p);blocks.erase(blocks.begin()+i);return;}}assert(false);}
void *MT_Alloc(int n,mtType_t){return Alloc(n);}
void MT_Free(byte *p,int n){Free(p,n);}
void *Hunk_AllocDebugMem(uint n){return Alloc(n);}
void Hunk_FreeDebugMem(void *p){for(const Block &b:blocks){if(b.p==p){Free(p,b.size);return;}}assert(false);}
bool Hunk_DataOnHunk(byte*){return false;}
void Hunk_AddData(int,void*,void*(*)(int)){assert(false);}
void SL_AddRefToString(uint){++refs;}
void SL_RemoveRefToStringOfSize(uint,uint){--refs;}
uint SL_GetStringOfSize(const char*,uint,uint,mtType_t){assert(false);return 0;}
const char *SL_ConvertToString(uint){return "bone";}
void XAnimResetAnimMap(const DObj_s*,uint){assert(false);}
void DObjDumpCreationInfo(DObjModel_s*,uint){}
int XModelGetBoneIndex(const XModel *m,uint name,uint base,byte *index){if(m->boneNames[0]==name){*index=(byte)base;return 1;}return 0;}
float Vec3Length(const float *p){return sqrtf(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);}
float Q_fabs(float f){return fabsf(f);}
'''
dobj=(root/'src/xanim/dobj.cpp').read_text()
source+=dobj[dobj.index('struct SavedDObjModel'):dobj.index('void __cdecl DObjInit')]
for name in ['DObjSetTree','DObjCreate','DObjCreateDuplicateParts','DObjComputeBounds','DObjFree','DObjGetCreateParms','DObjArchive','DObjUnarchive','DObjClone','DObjSetHidePartBits']:
    source+=extract(dobj,name)
text=(root/'src/xanim/xanim.cpp').read_text()
for name in ['XAnimCreateAnims','XAnimClone','XAnimFreeList','XAnimFreeAnims']:
    source+=extract(text,name)
text=(root/'src/xanim/xmodel_utils.cpp').read_text()
for name in ['XModelNumBones','XModelGetNumLods','XModelGetLodOutDist','XModelGetSurfaces']:
    source+=extract(text,name)
source+=extract((root/'src/xanim/xmodel.cpp').read_text(),'XModelGetRadius')
source+=extract((root/'src/xanim/xmodel_load_obj.cpp').read_text(),'XModelLoadCollData')
source+=r'''
template<class T> void put(std::vector<byte> &data,T value){const byte *p=(const byte*)&value;data.insert(data.end(),p,p+sizeof(T));}
int main()
{
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    for(int debug=0;debug<2;++debug)
    {
        g_anim_developer=debug;
        XAnim_s *a=XAnimCreateAnims("native",4096,Alloc);
        assert(a->size==4096);a->entries[4095].parts=(XAnimParts*)a;
        if(debug){assert(!a->debugAnimNames[4095]);}
        check();XAnimFreeAnims(a,Free);assert(blocks.empty());
    }
    XAnimParts from={};uint16_t names[2]={1,2};XAnimNotifyInfo note={3,0.5f};
    from.names=names;from.boneCount[9]=2;from.notifyCount=1;from.notify=&note;from.deltaPart=(XAnimDeltaPart*)&from;
    XAnimParts *copy=XAnimClone(&from,Alloc);assert(!memcmp(copy,&from,sizeof(XAnimParts))&&refs==3);Free(copy,sizeof(XAnimParts));refs=0;
    g_empty=1;
    XModel models[32]={};DObjModel_s inputs[32]={};uint16_t boneNames[32];
    XAnim_s anims={};XAnimTree_s tree={};tree.anims=&anims;
    for(int i=0;i<32;++i){boneNames[i]=(uint16_t)(i+1);models[i].name="model";models[i].numBones=1;models[i].numRootBones=1;models[i].boneNames=&boneNames[i];models[i].radius=1;inputs[i].model=&models[i];inputs[i].boneName=i?i:0;inputs[i].ignoreCollision=(i&1)!=0;}
    DObj_s obj={},clone={};
    DObjCreate(inputs,32,&tree,&obj,123);assert(obj.numBones==32&&obj.radius==32&&obj.ignoreCollision==0xAAAAAAAAu);
    for(int i=0;i<32;++i){assert(obj.models[i]==&models[i]);assert(((byte*)&obj.models[32])[i]==(i?i-1:255));}
    obj.hidePartBits[3]=0x12345678;
    DObjClone(&obj,&clone);assert(!clone.tree&&clone.models!=obj.models);assert(!memcmp(clone.models,obj.models,32*(sizeof(XModel*)+sizeof(byte))));
    DObjArchive(&obj);check();DObjUnarchive(&obj);check();
    assert(obj.tree==&tree&&obj.entnum==123&&obj.hidePartBits[3]==0x12345678&&obj.ignoreCollision==0xAAAAAAAAu);
    for(int i=0;i<32;++i){assert(obj.models[i]==&models[i]);assert(((byte*)&obj.models[32])[i]==(i?i-1:255));}
    DObjFree(&obj);DObjFree(&clone);assert(blocks.empty());
    XModel model={};XSurface surfaces[5]={};XSurface *selected=NULL;
    model.numLods=2;model.numsurfs=5;model.surfs=surfaces;model.lodInfo[0].numsurfs=3;model.lodInfo[1].surfIndex=3;model.lodInfo[1].numsurfs=2;model.lodInfo[1].dist=1234;
    assert(XModelGetLodOutDist(&model)==1234);assert(XModelGetSurfaces(&model,&selected,1)==2&&selected==surfaces+3);
    assert(XModelGetSurfaces(&model,&selected,4)==0&&!selected);
    std::vector<byte> data;put<int>(data,2);
    for(int s=0;s<2;++s){put<int>(data,1);for(int i=0;i<12;++i){put<float>(data,i==2?1.0f:0.0f);}for(int i=0;i<3;++i){put<float>(data,-1.0f);}for(int i=0;i<3;++i){put<float>(data,1.0f);}put<int>(data,s);put<int>(data,1);put<int>(data,7);}
    byte *pos=data.data();XModelLoadCollData(&pos,&model,Alloc,"fixture");assert(pos==data.data()+data.size());
    assert(model.numCollSurfs==2&&model.collSurfs[1].boneIdx==1&&model.collSurfs[1].collTris[0].plane[2]==1);check();
    for(int i=0;i<2;++i){Free(model.collSurfs[i].collTris,sizeof(XModelCollTri_s));}Free(model.collSurfs,2*sizeof(XModelCollSurf_s));assert(blocks.empty());
    puts("PASS: native animation trees/debug names/clones; 32-model DObj create/clone/archive/unarchive/free; LOD selection; raw collision surfaces; allocation canaries and exact free sizes");
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
