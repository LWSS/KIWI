"""Exercise production ui initialization and native pointer traversal."""
from pathlib import Path
import os
import re
import subprocess

root = Path.cwd()
out = Path(os.environ['TEMP'])/'kiwi-x64-epic/ui'
out.mkdir(parents=True, exist_ok=True)
vc = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
version = '10.0.26100.0'
includes = [root/'src', root/'deps', root/'deps/msslib', root/'build/_deps/tracy-src/public', vc/'include']
includes += [sdk/'Include'/version/n for n in ['ucrt', 'shared', 'um']]
includes += [Path('C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/include')]

def extract(file, name):
    text = (root/'src/ui'/file).read_text()
    match = re.search(r'^\w[^\n;]*\b'+name+r'\s*\(', text, re.M)
    return text[match.start():text.index('\n}', match.end())+2]+'\n'

source = r'''
#include <universal/q_shared.h>
#include <universal/com_memory.h>
#include <ui/ui_shared.h>
#include <ui/ui.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#undef static_assert
static_assert(sizeof(operandInternalDataUnion)==sizeof(void*));
static_assert(sizeof(expressionEntry)==(sizeof(void*)==8?24:12));
void MyAssertHandler(const char*,int,int,const char*,...){abort();}
void Com_Error(errorParm_t,const char*,...){throw 1;}
void Com_PrintError(int,const char*,...){}
void *Z_Malloc(int size,const char*,int){return calloc(1,size);}
void Z_Free(void *ptr,int){free(ptr);}
const char *CopyString(const char *s){return _strdup(s);}
void FreeString(const char *s){free((void*)s);}
int Com_sprintf(char *dst,unsigned int size,const char *fmt,...){va_list ap;va_start(ap,fmt);int n=vsnprintf(dst,size,fmt,ap);va_end(ap);return n;}
char *va(const char *fmt,...){static char buf[4096];va_list ap;va_start(ap,fmt);vsnprintf(buf,sizeof(buf),fmt,ap);va_end(ap);return buf;}
void UI_FilterStringForButtonAnimation(char*,size_t){}
int numtokens;
'''
obj=(root/'src/ui/ui_shared_obj.cpp').read_text()
for name in ['s_consumedOperandCount','s_precedence','s_rightToLeft']:
    m=re.search(r'^\w+ '+name+r'\[26\] =',obj,re.M)
    source+=obj[m.start():obj.index('};',m.end())+2]+'\n'
start=obj.index('union UIParserAllocationHeader')
source+=obj[start:obj.index('};',start)+2]+'\n'
for name in ['GetMemory','FreeMemory','GetClearedMemory','PC_CopyToken','PC_CopyDefine',
             'Statement_AddEntry','Statement_AddOperator','Statement_AddIntOperand',
             'Statement_AddFloatOperand','Statement_AddStringOperand','free_expression',
             'Eval_CanPushValue','Eval_PushInteger','Eval_IsUnaryOp',
             'Eval_PrepareBinaryOpSameTypes','Eval_PrepareBinaryOpIntegers',
             'Eval_PrepareBinaryOpBoolean','Eval_EvaluationStep','Eval_PushOperator',
             'Eval_PushNumber','Eval_AnyMissingOperands','Eval_Solve']:
    source+=extract('ui_shared_obj.cpp',name)
source+=(root/'src/ui/ui_localvars.cpp').read_text().replace('#include "ui_shared.h"', '#include <ui/ui_shared.h>')
for name in ['AddOperandToStack','GetOperand']:
    source+=extract('ui_expressions.cpp',name)
for name in ['UI_ReplaceConversions','UI_ReplaceConversionString','UI_ReplaceConversionInt','UI_ReplaceConversionInts']:
    source+=extract('ui_main.cpp',name)
source+=r'''
int main()
{
    token_s token={};token.next=&token;token.whitespace_p=(char*)&token;token.endwhitespace_p=token.whitespace_p+7;token.floatvalue=3.25;
    token_s *copy=PC_CopyToken(&token);
    assert((uintptr_t)copy%alignof(token_s)==0&&copy->whitespace_p==token.whitespace_p&&copy->floatvalue==3.25&&!copy->next);
    define_s def={};def.name=(char*)"NAME";def.tokens=&token;def.parms=&token;token.next=NULL;
    define_s *dc=PC_CopyDefine(NULL,&def);
    assert(!strcmp(dc->name,"NAME")&&dc->tokens!=&token&&dc->tokens->endwhitespace_p==token.endwhitespace_p&&dc->parms->whitespace_p==token.whitespace_p);
    FreeMemory((char*)dc->tokens);FreeMemory((char*)dc->parms);FreeMemory((char*)dc);FreeMemory((char*)copy);
    statement_s statement={};statement.entries=(expressionEntry**)Z_Malloc(600*sizeof(expressionEntry*),"test",0);
    Statement_AddStringOperand(&statement,(char*)"high pointer");Statement_AddIntOperand(&statement,123);Statement_AddFloatOperand(&statement,1.25f);
    assert(!strcmp(statement.entries[0]->data.operand.internals.string,"high pointer"));
    assert(statement.entries[1]->data.operand.internals.intVal==123&&statement.entries[2]->data.operand.internals.floatVal==1.25f);
    free_expression(&statement);assert(!statement.entries&&!statement.numEntries);
    OperandStack stack={};Operand in={},back={};in.dataType=VAL_STRING;in.internals.string=(char*)&token;
    for(int i=0;i<60;++i){AddOperandToStack(&stack,&in);}
    for(int i=0;i<60;++i){assert(GetOperand(&stack,&back)&&back.internals.string==in.internals.string);}
    UILocalVarContext locals={};
    for(int i=0;i<256;++i){char name[32];sprintf(name,"v%d",i);UILocalVar *v=UILocalVar_FindOrCreate(&locals,name);assert(v);UILocalVar_SetString(v,name);assert(!strcmp(UILocalVar_GetString(UILocalVar_Find(&locals,name),NULL,0),name));}
    assert(!UILocalVar_FindOrCreate(&locals,(char*)"overflow"));UILocalVar_Shutdown(&locals);
    char output[64];ConversionArguments args={};args.argCount=2;args.args[0]="100%";args.args[1]="native";
    UI_ReplaceConversions("&&2 &&1 &&0",&args,output,sizeof(output));assert(!strcmp(output,"native 100% &&0"));
    struct {char text[4];unsigned int canary;} bounded={{0},0xABCDEF01};
    UI_ReplaceConversions("&&1",&args,bounded.text,sizeof(bounded.text));assert(!strcmp(bounded.text,"100")&&bounded.canary==0xABCDEF01);
    int ints[3]={10,-20,30};assert(!strcmp(UI_ReplaceConversionInts("&&3 &&2 &&1",3,ints),"30 -20 10"));
    Eval e={};EvalValue result={};
    assert(Eval_PushInteger(&e,2)&&Eval_PushOperator(&e,EVAL_OP_PLUS)&&Eval_PushNumber(&e,3.5)&&Eval_PushOperator(&e,EVAL_OP_MULTIPLY)&&Eval_PushInteger(&e,4));
    Eval_Solve(&result,&e);assert(result.type==EVAL_VALUE_DOUBLE&&result.u.d==16);
    for(int condition=0;condition<2;++condition){
        e={};e.valStackPos=3;e.valStack[0].type=EVAL_VALUE_INT;e.valStack[0].u.i=condition;
        e.valStack[1].type=e.valStack[2].type=EVAL_VALUE_STRING;e.valStack[1].u.s=_strdup("yes");e.valStack[2].u.s=_strdup("no");
        e.opStackPos=2;e.opStack[0]=EVAL_OP_QUESTION;e.opStack[1]=EVAL_OP_COLON;
        Eval_Solve(&result,&e);assert(!strcmp(result.u.s,condition?"yes":"no"));free(result.u.s);
    }
    e={};e.valStackPos=2;e.valStack[0].type=e.valStack[1].type=EVAL_VALUE_STRING;
    e.valStack[0].u.s=_strdup("wide ");e.valStack[1].u.s=_strdup("pointer");e.opStackPos=1;e.opStack[0]=EVAL_OP_PLUS;
    Eval_Solve(&result,&e);assert(!strcmp(result.u.s,"wide pointer"));free(result.u.s);
    bool rejected=false;e={};try{Eval_Solve(&result,&e);}catch(int){rejected=true;}assert(rejected);
    rejected=false;e={};e.valStackPos=1024;try{Eval_PushInteger(&e,1);}catch(int){rejected=true;}assert(rejected);
    rejected=false;e={};Eval_PushInteger(&e,1);Eval_PushOperator(&e,EVAL_OP_DIVIDE);Eval_PushInteger(&e,0);
    try{Eval_Solve(&result,&e);}catch(int){rejected=true;}assert(rejected);
    puts("PASS: aligned token/macro copies, expression strings, operand stack, full local-variable table, substitutions, mixed arithmetic, string ternary/concat, invalid expressions");
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
