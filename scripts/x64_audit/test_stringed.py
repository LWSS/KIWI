"""Exercise production stringed initialization and native pointer traversal."""
from pathlib import Path
import os
import re
import subprocess

root = Path.cwd()
out = Path(os.environ['TEMP'])/'kiwi-x64-epic/stringed'
out.mkdir(parents=True, exist_ok=True)
vc = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
version = '10.0.26100.0'
includes = [root/'src', root/'deps', root/'deps/msslib', root/'build/_deps/tracy-src/public', vc/'include']
includes += [sdk/'Include'/version/n for n in ['ucrt', 'shared', 'um']]
includes += [Path('C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/include')]

def extract(file, name):
    text = (root/'src/stringed'/file).read_text()
    match = re.search(r'^\w[^\n;]*\b'+name+r'\s*\(', text, re.M)
    return text[match.start():text.index('\n}', match.end())+2]+'\n'

source = r'''
#include <universal/q_shared.h>
#include <stringed/stringed_ingame.h>
#include <stringed/stringed_hooks.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#undef static_assert
static_assert(sizeof(languageInfo_t)==(sizeof(void*)==8?16:8));
int iCurrString;
char szStrings[10][1024];
languageInfo_t g_languages[15];
const dvar_t *loc_warnings=NULL,*loc_warningsAsErrors=NULL;
void MyAssertHandler(const char*,int,int,const char*,...){abort();}
void Com_Error(errorParm_t,const char*,...){throw 1;}
void Com_Printf(int,const char*,...){}
void Com_PrintWarning(int,const char*,...){}
void I_strncpyz(char *dst,const char *src,int n){assert(n>0);memmove(dst,src,n-1);dst[n-1]=0;}
char *va(const char *fmt,...){static char buf[4096];va_list ap;va_start(ap,fmt);vsnprintf(buf,sizeof(buf),fmt,ap);va_end(ap);return buf;}
const char *SEH_StringEd_GetString(const char *reference){return reference;}
'''
for name in ['SEH_VerifyLanguageSelection','SEH_GetLocalizedTokenReference','SEH_LocalizeTextMessage']:
    source+=extract('stringed_hooks.cpp',name)
source+=r'''
int main()
{
    CStringEdPackage package;
    char input[]="REFERENCE TEST\nLANG_ENGLISH \"hello\"\r\nENDMARKER";
    char *cursor=input;
    struct {char text[64];unsigned int canary;} line={{0},0x12345678};
    assert(package.ReadLine(&cursor,line.text,sizeof(line.text))&&!strcmp(line.text,"REFERENCE TEST"));
    assert(package.ReadLine(&cursor,line.text,sizeof(line.text))&&!strcmp(line.text,"LANG_ENGLISH \"hello\""));
    assert(package.ReadLine(&cursor,line.text,sizeof(line.text))&&!strcmp(line.text,"ENDMARKER"));
    assert(!package.ReadLine(&cursor,line.text,sizeof(line.text))&&line.canary==0x12345678);
    char overflow[100];memset(overflow,'a',99);overflow[99]=0;cursor=overflow;
    bool rejected=false;try{package.ReadLine(&cursor,line.text,sizeof(line.text));}catch(int){rejected=true;}assert(rejected&&line.canary==0x12345678);
    std::string s;package.InsideQuotes(&s,"\"   ");assert(s.empty());
    package.InsideQuotes(&s,"\"hello\"  ");assert(s=="hello");
    char good[]="&&1 &&9",zero[]="&&0",duplicate[]="&&2 &&2",bad[]="&&x";
    assert(package.IsStringFormatCorrect(good));assert(!package.IsStringFormatCorrect(zero));
    assert(!package.IsStringFormatCorrect(duplicate)&&!package.IsStringFormatCorrect(bad));
    assert(!strcmp(SEH_LocalizeTextMessage("hello","test",LOCMSG_SAFE),"hello"));
    assert(!strcmp(SEH_LocalizeTextMessage("hello &&1\x15world","test",LOCMSG_SAFE),"hello world"));
    std::string longText(2000,'a');assert(!SEH_LocalizeTextMessage(longText.c_str(),"test",LOCMSG_SAFE));
    char output[5];assert(!SEH_GetLocalizedTokenReference(output,sizeof(output),"12345","test",LOCMSG_SAFE));
    assert(SEH_GetLocalizedTokenReference(output,sizeof(output),"1234","test",LOCMSG_SAFE)&&!strcmp(output,"1234"));
    g_languages[2].bPresent=1;assert(SEH_VerifyLanguageSelection(-1)==2&&SEH_VerifyLanguageSelection(99)==2);
    puts("PASS: native line parsing, LF/CRLF/final line, overflow canary, empty quotes, format validation, localization insertion/bounds");
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
