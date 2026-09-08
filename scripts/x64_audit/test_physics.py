"""Native physics integration regression probes using production functions."""
from pathlib import Path
import os
import re
import subprocess

root = Path.cwd()
out = Path(os.environ['TEMP'])/'kiwi-x64-epic/physics'
out.mkdir(parents=True, exist_ok=True)
vc = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
version = '10.0.26100.0'
includes = [root/'src', root/'deps', root/'deps/msslib', root/'build/_deps/tracy-src/public', vc/'include']
includes += [sdk/'Include'/version/n for n in ['ucrt', 'shared', 'um']]
includes += [Path('C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/include')]

def extract(file, name):
    s = (root/'src/physics'/file).read_text()
    m = re.search(r'^\w[^\n;]*\b'+name+r'\s*\(', s, re.M)
    return s[m.start():s.index('\n}', m.end())+2]+'\n'

source = r'''
#include <universal/q_shared.h>
#include <physics/phys_local.h>
#include <physics/ode/collision_kernel.h>
#include <physics/ode/stack.h>
#include <universal/profile.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#undef PROF_SCOPED
#define PROF_SCOPED(x)
#undef static_assert
static_assert(sizeof(BodyState)==112);
static_assert(sizeof(Jitter)==36);
static_assert(sizeof(BrushInfo)<=sizeof(((dxUserGeom*)0)->user_data));
static_assert(sizeof(dxUserGeom)<=sizeof(dxGeomTransform));
static_assert(sizeof(PhysObjUserData)==(sizeof(void*)==8?120:112));
PhysGlob physGlob;
static byte archive[1024];
static size_t cursor;
static bool writing;
void MemFile_ArchiveData(MemoryFile*,int bytes,void *data)
{
    assert(cursor+bytes<=sizeof(archive));
    if(writing) { memcpy(archive+cursor,data,bytes); }
    else { memcpy(data,archive+cursor,bytes); }
    cursor+=bytes;
}
static void Callback() {}
static int called;
static const objInfo *expectedInput;
static Results *expectedOutput;
static void Check(const objInfo *input,Results *output,int type)
{
    assert(input==expectedInput&&output==expectedOutput);
    called=type;
}
void Phys_CollideBoxWithBrush(const cbrush_t*,const objInfo*i,Results*r){Check(i,r,1);}
void Phys_CollideOrientedBrushModelWithBrush(const cbrush_t*,const objInfo*i,Results*r){Check(i,r,2);}
void Phys_CollideOrientedBrushWithBrush(const cbrush_t*b,const cbrush_t*,const objInfo*i,Results*r){assert(b==i->u.brush);Check(i,r,3);}
void Phys_CollideCylinderWithBrush(const cbrush_t*,const objInfo*i,Results*r){Check(i,r,4);}
void Phys_CollideCapsuleWithBrush(const cbrush_t*,const objInfo*i,Results*r){Check(i,r,5);}
static BrushTrimeshData expectedMesh;
void Phys_CollideOrientedBrushWithTriangleList(const cbrush_t*,const uint16_t *indices,const float (*verts)[3],int count,const objInfo *input,int flags,Results *results)
{
    assert(indices==expectedMesh.indices&&verts==expectedMesh.verts&&count==expectedMesh.triCount);
    assert(input==expectedMesh.input&&flags==expectedMesh.surfaceFlags&&results==expectedMesh.results);
    called=6;
}
void *(__cdecl *physAlloc)(int);
static void *Allocate(int size){return malloc(size);}
static GeomState captured;
void dMassSetZero(dMass *m){memset(m,0,sizeof(dMass));}
void dBodyGetMass(dBodyID,dMass *m){memset(m,0,sizeof(dMass));}
void Phys_BodyAddGeomAndSetMass(PhysWorld,dxBody*,float,GeomState*g,const float*){captured=*g;}
'''
source += extract('phys_ode.cpp', 'Phys_ArchiveState')
source += extract('phys_ode.cpp', 'Phys_ObjAddGeomBrush')
source += extract('phys_ode.cpp', 'Phys_ObjAddGeomBrushModel')
source += extract('phys_world_collision.cpp', 'Phys_TestGeomInBrush')
source += extract('phys_coll_boxbrush.cpp', 'Phys_CollideOrientedBrushWithBrush_Wrapper')
source += extract('phys_coll_boxbrush.cpp', 'Phys_CollideOrientedBrushWithTriangleList_Wrapper')
preset = (root/'src/physics/physpreset_load_obj.cpp').read_text()
source += preset[preset.index('struct PhysPresetLite'):preset.index('void __cdecl PhysPreset_Strcpy')]
source += extract('physpreset_load_obj.cpp', 'PhysPreset_Strcpy')
source += r'''
int main()
{
    if(sizeof(void*)==8) { assert((uintptr_t)&physGlob>0xffffffffu); }
    PhysStaticArray<dxJointBall,160> pool;
    pool.init();
    for(int i=0;i<160;i++) { assert(pool.allocate()==&pool.entries[i]); }
    assert(!pool.allocate());
    pool.release(&pool.entries[77]);
    assert(pool.allocate()==&pool.entries[77]);
    objInfo input={};
    Results output={};
    InputOutput io={&input,&output};
    expectedInput=&input;expectedOutput=&output;output.maxContacts=1;
    input.u.brush=(cbrush_t*)&physGlob;
    PhysPresetLite preset={};physAlloc=Allocate;
    preset.piecesSpreadFraction=12.5f;
    PhysPreset_Strcpy((byte*)&preset+physPresetFields[6].iOffset,"stone");
    assert(strcmp(preset.sndAliasPrefix,"stone")==0&&preset.piecesSpreadFraction==12.5f);
    free((void*)preset.sndAliasPrefix);
    PhysPreset_Strcpy((byte*)&preset+physPresetFields[6].iOffset,"");
    assert(!preset.sndAliasPrefix[0]);
    assert(physPresetFields[7].iOffset==offsetof(PhysPresetLite,piecesSpreadFraction));
    BrushBrushData brushData={input.u.brush,&input,&output};called=0;
    Phys_CollideOrientedBrushWithBrush_Wrapper(input.u.brush,&brushData);assert(called==3);
    expectedMesh.indices=(const uint16_t*)&physGlob;expectedMesh.verts=(const float (*)[3])&input;
    expectedMesh.triCount=13;expectedMesh.input=&input;expectedMesh.surfaceFlags=123;expectedMesh.results=&output;called=0;
    Phys_CollideOrientedBrushWithTriangleList_Wrapper(input.u.brush,&expectedMesh);assert(called==6);
    for(int type=1;type<=5;type++)
    {
        input.type=(PhysicsGeomType)type;called=0;
        Phys_TestGeomInBrush(NULL,&io);assert(called==type);
        output.contactCount=1;called=0;
        Phys_TestGeomInBrush(NULL,&io);assert(!called);
        output.contactCount=0;
    }
    PhysMass mass={};
    mass.momentsOfInertia[0]=1;mass.momentsOfInertia[1]=2;mass.momentsOfInertia[2]=3;
    Phys_ObjAddGeomBrush(PHYS_WORLD_DYNENT,(dxBody*)&physGlob,input.u.brush,&mass);
    assert(captured.u.brushState.u.brush==input.u.brush);
    assert(memcmp(captured.u.brushState.momentsOfInertia,mass.momentsOfInertia,3*sizeof(float))==0);
    Phys_ObjAddGeomBrushModel(PHYS_WORLD_DYNENT,(dxBody*)&physGlob,37,&mass);
    assert(captured.u.brushState.u.brushModel==37);
    assert(memcmp(captured.u.brushState.momentsOfInertia,mass.momentsOfInertia,3*sizeof(float))==0);
    for(int i=0;i<3;i++)
    {
        PhysWorldData *w=&physGlob.worldData[i];
        w->timeLastSnapshot=100+i;w->timeLastUpdate=200+i;w->timeNowLerpFrac=.25f;
        w->collisionCallback=Callback;w->numJitterRegions=5;w->useContactCentroids=true;
        w->jitterRegions[4].maxDisplacement=10+i;
    }
    physGlob.gravityDirection[2]=-1;writing=true;Phys_ArchiveState(NULL);
    assert(cursor==624);
    for(int i=0;i<3;i++)
    {
        assert(*(uint32_t*)(archive+204*i+12)==0);
        // A legacy saved address must not replace the native registration.
        *(uint32_t*)(archive+204*i+12)=0x12345678;
        memset(&physGlob.worldData[i],0,sizeof(PhysWorldData));
        physGlob.worldData[i].collisionCallback=Callback;
    }
    cursor=0;writing=false;Phys_ArchiveState(NULL);
    for(int i=0;i<3;i++)
    {
        PhysWorldData *w=&physGlob.worldData[i];
        assert(w->collisionCallback==Callback&&w->timeLastUpdate==200+i);
        assert(w->numJitterRegions==5&&w->jitterRegions[4].maxDisplacement==10+i);
    }
    assert(cursor==624&&physGlob.gravityDirection[2]==-1);
    char *arena=(char*)VirtualAlloc(NULL,65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(arena);dStack stack={};stack.base=stack.pointer=arena;stack.size=stack.committed=65536;stack.pagesize=4096;
    stack.alloc(1);char *frame=stack.pushFrame();assert(((uintptr_t)frame%sizeof(void*))==0);
    stack.alloc(3);assert(stack.popFrame()==frame);VirtualFree(arena,0,MEM_RELEASE);
    puts("PASS: physics native layouts, pool reuse, pointer callbacks, brush geometry, legacy archive layout, stack alignment");
}
'''
test = out/'regression.cpp'
test.write_text(source)
probe = out/'regression-diagnostic.h'
probe.write_text('#define static_assert(...)\n')
for arch in ['x86', 'x64']:
    directory = out/arch
    directory.mkdir(exist_ok=True)
    args = ['/nologo', '/O2', '/std:c++20', '/EHsc', '/DWIN32', '/DKISAK_SP',
            '/D_ALLOW_KEYWORD_MACROS', '/DCPUSTRING="audit"', '/FI'+str(probe)]
    args += ['/I'+str(p) for p in includes]
    args += ['/Fe'+str(directory/'regression.exe'), '/Fo'+str(directory/'regression.obj'), str(test), '/link']
    args += ['/LIBPATH:'+str(vc/f'lib/{arch}')]
    args += ['/LIBPATH:'+str(sdk/'Lib'/version/n/arch) for n in ['ucrt', 'um']]
    result = subprocess.run([str(vc/f'bin/Hostx64/{arch}/cl.exe')]+args, capture_output=True, text=True)
    (directory/'build.txt').write_text(result.stdout+result.stderr)
    assert result.returncode==0, result.stdout+result.stderr
    result = subprocess.run([str(directory/'regression.exe')], capture_output=True, text=True)
    (directory/'run.txt').write_text(result.stdout+result.stderr)
    assert result.returncode==0, result.stdout+result.stderr
    print(arch,result.stdout.strip())
