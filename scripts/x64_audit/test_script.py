"""Test native script values, bytecode operands and suspended stacks."""
from pathlib import Path
import os
import re
import subprocess

root = Path.cwd()
out = Path(os.environ['TEMP'])/'kiwi-x64-epic/script-regression'
out.mkdir(parents=True, exist_ok=True)
vc = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
version = '10.0.26100.0'
includes = [root/'src', root/'deps', root/'deps/msslib', root/'build/_deps/tracy-src/public', vc/'include']
includes += [sdk/'Include'/version/n for n in ['ucrt', 'shared', 'um']]
includes += [Path('C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/include')]


def extract(file, name):
    text = (root/'src/script'/file).read_text()
    match = re.search(r'^\w[^\n;]*\b'+name+r'\s*\(', text, re.M)
    assert match,name
    return text[match.start():text.index('\n}',match.end())+2]+'\n'

source = r'''
#include <universal/q_shared.h>
#include <script/scr_compiler.h>
#include <script/scr_main.h>
#include <script/scr_vm.h>
#include <script/scr_memorytree.h>
#include <script/scr_parsetree.h>
#include <script/scr_readwrite.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#undef static_assert
static_assert(sizeof(VariableValue)==(sizeof(void*)==8?16:8));
static_assert(sizeof(VariableValueInternal)==(sizeof(void*)==8?24:16));
static_assert(sizeof(sval_u)==sizeof(void*));
scrCompileGlob_t scrCompileGlob;
scrCompilePub_t scrCompilePub;
scrVmPub_t scrVmPub;
scrVmDebugPub_t scrVmDebugPub;
scrVarPub_t scrVarPub;
scrMemTreeGlob_t scrMemTreeGlob;
function_stack_t fs;
HunkUser *g_allocNodeUser=(HunkUser*)1;
static __declspec(align(16)) byte arena[65536];
static uint used;
void *Hunk_UserAlloc(HunkUser*,uint size,int alignment)
{
    used=(used+alignment-1)&~(alignment-1);
    void *result=arena+used;used+=size;assert(used<=sizeof(arena));return result;
}
char *TempMallocAlignStrict(uint size){return (char*)Hunk_UserAlloc(NULL,size,1);}
void MyAssertHandler(const char*,int,int,const char*,...){abort();}
void Com_Error(errorParm_t,const char*,...){abort();}
void MT_Error(const char*,int){abort();}
static byte *allocation;
static int allocationSize;
void *MT_Alloc(int size,mtType_t){allocationSize=size;allocation=(byte*)malloc(size+16);memset(allocation,0xa5,size+16);return allocation;}
void MT_Free(byte *p,int size){assert(p==allocation&&size==allocationSize);for(int i=0;i<16;i++){assert(p[size+i]==0xa5);}free(p);allocation=NULL;}
void AddRefToObject(uint){}
uint GetParentLocalId(uint id){assert(id>1);return id-1;}
void Scr_ClearWaitTime(uint){}
void Scr_ResetTimeout(){}
int Scr_AddLocalVars(uint){return 0;}
void AddOpcodePos(uint,int){}
void EmitOpcode(uint opcode,int,int){*TempMallocAlignStrict(1)=(char)opcode;}
void EmitByte(byte value){*TempMallocAlignStrict(1)=(char)value;}
void EmitShort(short value){memcpy(TempMallocAlignStrict(sizeof(short)),&value,sizeof(short));}
static byte saveBytes[4096];
static int saveSize,saveRead;
static unsigned int g_idHistoryIndex;
static short idHistory[16];
void MemFile_WriteData(MemoryFile*,int size,const void *data){assert(saveSize+size<=sizeof(saveBytes));memcpy(saveBytes+saveSize,data,size);saveSize+=size;}
void MemFile_ReadData(MemoryFile*,int size,byte *data){assert(saveRead+size<=saveSize);memcpy(data,saveBytes+saveRead,size);saveRead+=size;}
double MemFile_ReadFloat(MemoryFile *file){float value;MemFile_ReadData(file,sizeof(float),(byte*)&value);return value;}
void MemFile_WriteCString(MemoryFile*,const char*){abort();}
int Scr_ReadString(MemoryFile*){abort();}
const char *SL_ConvertToString(uint){abort();}
char *va(const char*,...){abort();}
static float loadedVector[3];
const float *Scr_AllocVector(const float *v){memcpy(loadedVector,v,sizeof(loadedVector));return loadedVector;}
bool Scr_IsInOpcodeMemory(const char *p){return p>=(char*)arena&&p<(char*)arena+sizeof(arena);}
char g_EndPos;
void Scr_ClearErrorMessage(){scrVarPub.error_message=NULL;}
void AddRefToValue(int,VariableUnion){}
void Scr_CastBool(VariableValue *v){v->u.intValue=v->u.intValue!=0;v->type=VAR_INTEGER;}
bool IsValidArrayIndex(uint value){return value<0x800000;}
uint GetInternalVariableIndex(uint value){return value+0x800000;}
float *Scr_AllocVector(){return loadedVector;}
void RemoveRefToVector(const float*){}
void Scr_CastWeakerPair(VariableValue *a,VariableValue *b){assert(a->type==b->type);}
void Scr_CastWeakerStringPair(VariableValue *a,VariableValue *b){assert(a->type==b->type);}
void Scr_UnmatchingTypesError(VariableValue*,VariableValue*){abort();}
void Scr_Error(const char*){abort();}
int SL_GetStringLen(uint){abort();}
uint SL_GetStringOfSize(const char*,uint,uint,mtType_t){abort();}
void SL_RemoveRefToString(uint){}
'''
for file,names in {
    'scr_parsetree.cpp':['Scr_AllocNode','node0','node1','node2','node3'],
    'scr_compiler2.cpp':['EmitCodepos','EmitNativeValue','EmitFloat','EmitInteger','EmitGetInteger','AddFunction','CompareCaseInfo'],
    'scr_vm.cpp':['Scr_ReadCodePos','Scr_ReadUnsigned','Scr_ReadInt','Scr_ReadFloat','Scr_GetReturnPos','Scr_GetNextCodepos','VM_ArchiveStack2','VM_UnarchiveStack2'],
    'scr_memorytree.cpp':['MT_InitBits','MT_GetSize'],
    'scr_variable.cpp':['Scr_EvalPlus','Scr_EvalMinus','Scr_EvalMultiply','Scr_EvalDivide'],
    'scr_readwrite.cpp':['WriteByte','WriteShort','WriteInt','WriteFloat','WriteVector','Scr_ReadVec3','WriteCodepos','Scr_ReadCodepos','Scr_CheckIdHistory','WriteId','Scr_ReadId','DoSaveEntryInternal','Scr_DoLoadEntryInternal','WriteStack','Scr_ReadStack'],
}.items():
    for name in names:
        source+=extract(file,name)
source+=r'''
static void Builtin(){}
int main(int argc,char **argv)
{
    assert(argc==2);
    MT_InitBits();
    for(int n=1;n<=64;n++)
    {
        int size=MT_GetSize(n);assert((sizeof(MemoryNode)<<size)>=n);
        if(sizeof(void*)==8){assert(size>=1);}
    }
    sval_u ptr;ptr.node=(sval_u*)arena;
    if(sizeof(void*)==8){assert((uintptr_t)ptr.node>UINT32_MAX);}
    sval_u copy;copy=ptr;assert(copy.node==ptr.node);
    sval_u tree=node3(ENUM_vector,ptr,123,ptr);
    assert(tree.node[1].node==ptr.node&&tree.node[2].intValue==123&&tree.node[3].node==ptr.node);
    assert((uintptr_t)tree.node%alignof(sval_u)==0);
    used=0;EmitGetInteger(0x12345678,0);assert(used==5&&arena[0]==OP_GetInteger);
    const char *read=(char*)arena+1;assert(Scr_ReadInt(&read)==0x12345678&&read==(char*)arena+used);
    used=0;EmitCodepos((char*)arena);read=(char*)arena;assert(Scr_ReadCodePos(&read)==(char*)arena&&read==(char*)arena+used);
    used=0;EmitNativeValue(123);read=(char*)arena;assert(Scr_ReadUnsigned(&read)==123&&read==(char*)arena+used);
    used=0;EmitFloat(3.25f);read=(char*)arena;assert(Scr_ReadFloat(&read)==3.25f&&read==(char*)arena+used);
    assert(AddFunction((uintptr_t)Builtin,"test")==0);
    assert(AddFunction((uintptr_t)Builtin,"test")==0&&scrCompilePub.func_table_size==1);
    ((void(*)())scrCompilePub.func_table[0])();
    uintptr_t cases[3][2]={{3,(uintptr_t)arena},{1,(uintptr_t)arena+1},{2,(uintptr_t)arena+2}};
    qsort(cases,3,2*sizeof(uintptr_t),CompareCaseInfo);
    assert(cases[0][0]==3&&cases[1][0]==2&&cases[2][0]==1);
    struct Operand {int opcode;size_t size;} operands[]={
        {OP_GetInteger,sizeof(int)},{OP_GetFloat,sizeof(float)},{OP_GetAnimation,sizeof(uintptr_t)},
        {OP_GetFunction,sizeof(void*)},{OP_ScriptThreadCall,2*sizeof(uintptr_t)},
        {OP_ScriptThreadCallPointer,sizeof(uintptr_t)},{OP_object,2*sizeof(uintptr_t)},
        {OP_GetVector,3*sizeof(float)}};
    scrVarPub.evaluate=true;scrVmPub.function_frame=scrVmPub.function_frame_start;
    VariableValue top;top.type=VAR_INTEGER;top.u.intValue=1;uint debugLocal=1;
    for(unsigned int i=0;i<ARRAY_COUNT(operands);i++)
    {
        memset(arena,0,64);
        assert(Scr_GetNextCodepos(&top,(char*)arena,operands[i].opcode,0,&debugLocal)==(char*)arena+1+operands[i].size);
    }
    memset(arena,0,64);
    assert(Scr_GetNextCodepos(&top,(char*)arena,OP_jump,0,&debugLocal)==(char*)arena+1+sizeof(uintptr_t));
    VariableValue values[4];
    values[1].type=VAR_VECTOR;values[1].u.vectorValue=(float*)arena;
    values[2].type=VAR_INTEGER;values[2].u.intValue=-1234567;
    values[3].type=VAR_FUNCTION;values[3].u.codePosValue=(char*)arena+10;
    uint locals[4]={};scrVmPub.localVars=locals;
    scrVmPub.function_count=1;scrVmPub.function_frame=&scrVmPub.function_frame_start[1];
    uint localId=1;
    VariableStackBuffer *stack=VM_ArchiveStack2(3,(char*)arena+20,values+3,0,&localId);
    assert(stack->bufLen==SCR_STACK_HEADER_SIZE+3*SCR_STACK_VALUE_SIZE);
    assert(stack->buf[0]==VAR_VECTOR&&Scr_ReadStackValue(stack->buf+1).vectorValue==(float*)arena);
    assert(Scr_ReadStackValue(stack->buf+SCR_STACK_VALUE_SIZE+1).intValue==-1234567);
    assert(Scr_ReadStackValue(stack->buf+2*SCR_STACK_VALUE_SIZE+1).codePosValue==(char*)arena+10);
    fs.startTop=scrVmPub.stack;scrVmPub.function_count=0;scrVmPub.function_frame=scrVmPub.function_frame_start;
    scrVmPub.stack[0].type=VAR_CODEPOS;
    VM_UnarchiveStack2(1,&fs,stack);
    assert(!allocation&&fs.top==scrVmPub.stack+3&&fs.pos==(char*)arena+20);
    for(int i=1;i<=3;i++){assert(scrVmPub.stack[i].type==values[i].type&&scrVmPub.stack[i].u.nativeValue==values[i].u.nativeValue);}
    scrVarPub.programBuffer=(char*)arena;
    scrVarPub.saveIdMap[1]=1;scrVarPub.saveIdMapRev[1]=1;
    float vector[3]={1.25f,-2.5f,3.75f};
    values[1].u.vectorValue=vector;
    scrVmPub.function_count=1;scrVmPub.function_frame=&scrVmPub.function_frame_start[1];
    stack=VM_ArchiveStack2(3,(char*)arena+20,values+3,0,&localId);
    WriteStack(stack,NULL);MT_Free((byte*)stack,stack->bufLen);
    memset(idHistory,0,sizeof(idHistory));g_idHistoryIndex=0;
    stack=Scr_ReadStack(NULL);assert(saveRead==saveSize&&saveSize==31);
    assert(stack->size==3&&stack->localId==1&&stack->pos==(char*)arena+20);
    assert(memcmp(Scr_ReadStackValue(stack->buf+1).vectorValue,vector,sizeof(vector))==0);
    assert(Scr_ReadStackValue(stack->buf+SCR_STACK_VALUE_SIZE+1).intValue==-1234567);
    assert(Scr_ReadStackValue(stack->buf+2*SCR_STACK_VALUE_SIZE+1).codePosValue==(char*)arena+10);
    MT_Free((byte*)stack,stack->bufLen);
    FILE *wire=fopen(argv[1],"wb");assert(wire);fwrite(saveBytes,1,saveSize,wire);fclose(wire);
    values[2].type=VAR_CODEPOS;values[2].u.codePosValue=NULL;
    scrVmPub.function_count=2;scrVmPub.function_frame=&scrVmPub.function_frame_start[2];
    scrVmPub.function_frame_start[1].fs.pos=(char*)arena+30;
    localId=2;stack=VM_ArchiveStack2(3,(char*)arena+20,values+3,0,&localId);
    assert(localId==1&&stack->localId==2);
    assert(Scr_ReadStackValue(stack->buf+SCR_STACK_VALUE_SIZE+1).codePosValue==(char*)arena+30);
    scrVmPub.function_count=0;scrVmPub.function_frame=scrVmPub.function_frame_start;
    VM_UnarchiveStack2(1,&fs,stack);
    assert(scrVmPub.function_count==2&&scrVmPub.function_frame_start[1].fs.pos==(char*)arena+30);
    void (*operations[])(VariableValue*,VariableValue*)={Scr_EvalPlus,Scr_EvalMinus,Scr_EvalMultiply,Scr_EvalDivide};
    float left[3]={8,12,16},right[3]={2,3,4};
    float expected[4][3]={{10,15,20},{6,9,12},{16,36,64},{4,4,4}};
    for(int i=0;i<4;i++)
    {
        VariableValue a,b;a.type=b.type=VAR_VECTOR;a.u.vectorValue=left;b.u.vectorValue=right;
        operations[i](&a,&b);assert(a.type==VAR_VECTOR&&memcmp(a.u.vectorValue,expected[i],sizeof(left))==0);
    }
    puts("PASS: parse nodes, builtins, bytecode widths, native stack and save roundtrips, canaries");
}
'''
test = out/'regression.cpp'
test.write_text(source)
probe = out/'regression-diagnostic.h'
probe.write_text('#define static_assert(...)\n')
for arch in ['x86','x64']:
    directory = out/arch
    directory.mkdir(exist_ok=True)
    args = ['/nologo','/O2','/std:c++20','/EHsc','/DWIN32','/DKISAK_SP','/DRELEASE_ASSERTS',
            '/D_ALLOW_KEYWORD_MACROS','/DCPUSTRING="audit"','/FI'+str(probe)]
    args += ['/I'+str(p) for p in includes]
    args += ['/Fe'+str(directory/'regression.exe'),'/Fo'+str(directory/'regression.obj'),str(test),'/link']
    args += ['/LIBPATH:'+str(vc/f'lib/{arch}')]
    args += ['/LIBPATH:'+str(sdk/'Lib'/version/n/arch) for n in ['ucrt','um']]
    result = subprocess.run([str(vc/f'bin/Hostx64/{arch}/cl.exe')]+args,capture_output=True,text=True)
    (directory/'build.txt').write_text(result.stdout+result.stderr)
    assert result.returncode==0,result.stdout+result.stderr
    result = subprocess.run([str(directory/'regression.exe'),str(directory/'save.bin')],capture_output=True,text=True)
    (directory/'run.txt').write_text(result.stdout+result.stderr)
    assert result.returncode==0,result.stdout+result.stderr
    print(arch,result.stdout.strip())
assert (out/'x86/save.bin').read_bytes()==(out/'x64/save.bin').read_bytes()
print('PASS: byte-identical x86/x64 saved stack')
