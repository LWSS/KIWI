"""Exercise production ragdoll initialization and native pointer traversal."""
from pathlib import Path
import os
import re
import subprocess

root = Path.cwd()
out = Path(os.environ['TEMP'])/'kiwi-x64-epic/ragdoll'
out.mkdir(parents=True, exist_ok=True)
vc = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
version = '10.0.26100.0'
includes = [root/'src', root/'deps', root/'deps/msslib', root/'build/_deps/tracy-src/public', vc/'include']
includes += [sdk/'Include'/version/n for n in ['ucrt', 'shared', 'um']]
includes += [Path('C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/include')]

def extract(file, name):
    text = (root/'src/ragdoll'/file).read_text()
    match = re.search(r'^\w[^\n;]*\b'+name+r'\s*\(', text, re.M)
    return text[match.start():text.index('\n}', match.end())+2]+'\n'

source = r'''
#include <universal/q_shared.h>
#include <ragdoll/ragdoll.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#undef static_assert
static_assert(sizeof(Joint)==2*sizeof(void*));
static_assert(sizeof(Bone)==(sizeof(void*)==8?32:28));
static_assert(sizeof(StateEnt)==3*sizeof(void*));
RagdollBody ragdollBodies[32];
RagdollDef ragdollDefs[2];
BOOL ragdollInited;
bool ragdollFirstInit=true;
static dvar_t enable,maximum;
const dvar_t *ragdoll_enable=&enable,*ragdoll_max_simulating=&maximum;
void MyAssertHandler(const char*,int,int,const char*,...){abort();}
bool Sys_IsMainThread(){return true;}
void Dvar_SetInt(dvar_t *var,int value){var->current.integer=value;}
char Ragdoll_BodyNewState(RagdollBody *body,BodyState_t state){body->state=state;return 1;}
static void *expected[56];
static int destroyed;
void Phys_JointDestroy(PhysWorld world,dxJointHinge *joint){assert(world==PHYS_WORLD_RAGDOLL);assert(joint==expected[destroyed++]);}
void Phys_ObjDestroy(PhysWorld world,dxBody *body){assert(world==PHYS_WORLD_RAGDOLL);assert(body==expected[destroyed++]);}
static bool invalidSecond;
int DObjGetBoneIndex(const DObj_s*,unsigned int name,unsigned char *index){*index=(name==2&&invalidSecond)?255:(unsigned char)name;return 1;}
DObj_s *Ragdoll_BodyDObj(RagdollBody *body){return body->obj;}
RagdollDef *Ragdoll_BodyDef(RagdollBody *body){return &ragdollDefs[body->ragdollDef];}
void Ragdoll_SnapshotBaseLerpOffsets(RagdollBody*){}
'''
for file, names in {
    'ragdoll.cpp':['Ragdoll_Init','Ragdoll_InitBody','Ragdoll_GetUnusedBody'],
    'ragdoll_controller.cpp':['Ragdoll_HandleBody','Ragdoll_BodyBoneOrientations','Ragdoll_BodyPrevBoneOrientations'],
    'ragdoll_update.cpp':['Ragdoll_DestroyPhysJoints','Ragdoll_DestroyPhysObjs','Ragdoll_ExitDObjWait'],
}.items():
    for name in names:
        source += extract(file,name)
source += r'''
int main()
{
    memset(ragdollBodies,0xa5,sizeof(ragdollBodies));
    enable.current.enabled=true;maximum.current.integer=1;
    Ragdoll_Init();assert(ragdollInited&&maximum.current.integer==8);
    for(size_t i=0;i<sizeof(ragdollBodies);i++){assert(((byte*)ragdollBodies)[i]==0);}
    for(int i=0;i<32;i++){assert(Ragdoll_GetUnusedBody()==i+1);assert(Ragdoll_HandleBody(i+1)==&ragdollBodies[i]);}
    assert(!Ragdoll_GetUnusedBody());
    RagdollBody *body=&ragdollBodies[31];
    for(int i=0;i<2;i++){body->curOrientationBuffer=i;assert(Ragdoll_BodyBoneOrientations(body)==body->boneOrientations[i]);assert(Ragdoll_BodyPrevBoneOrientations(body)==body->boneOrientations[i^1]);}
    if(sizeof(void*)==8){assert((uintptr_t)body>UINT32_MAX);}
    body->numJoints=28;
    for(int i=0;i<28;i++){expected[2*i]=body->joints[i].joint=(byte*)body+2*i;expected[2*i+1]=body->joints[i].joint2=(byte*)body+2*i+1;}
    Ragdoll_DestroyPhysJoints(body);assert(destroyed==56);
    for(int i=0;i<28;i++){assert(!body->joints[i].joint&&!body->joints[i].joint2);}
    destroyed=0;body->numBones=14;
    for(int i=0;i<14;i++){expected[i]=body->bones[i].rigidBody=(dxBody*)((byte*)body+i);}
    Ragdoll_DestroyPhysObjs(body);assert(destroyed==14);
    for(int i=0;i<14;i++){assert(!body->bones[i].rigidBody);}
    body->numBones=1;body->obj=(DObj_s*)body;
    ragdollDefs[0].boneDefs[0].animBoneNames[0]=1;ragdollDefs[0].boneDefs[0].animBoneNames[1]=2;
    invalidSecond=true;assert(!Ragdoll_ExitDObjWait(body,BS_DOBJ_WAIT,BS_VELOCITY_CAPTURE));
    invalidSecond=false;assert(Ragdoll_ExitDObjWait(body,BS_DOBJ_WAIT,BS_VELOCITY_CAPTURE));
    assert(body->bones[0].animBones[1]==2);
    puts("PASS: full array reset, pool exhaustion, native body/joint traversal, orientation buffers, endpoint validation");
}
'''
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
