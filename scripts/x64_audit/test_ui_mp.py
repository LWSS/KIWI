"""Exercise production ui_mp initialization and native pointer traversal."""
from pathlib import Path
import os
import re
import subprocess

root = Path.cwd()
out = Path(os.environ['TEMP'])/'kiwi-x64-epic/ui_mp'
out.mkdir(parents=True, exist_ok=True)
vc = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
version = '10.0.26100.0'
includes = [root/'src', root/'deps', root/'deps/msslib', root/'build/_deps/tracy-src/public', vc/'include']
includes += [sdk/'Include'/version/n for n in ['ucrt', 'shared', 'um']]
includes += [Path('C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/include')]

def extract(file, name):
    text = (root/'src/ui_mp'/file).read_text()
    match = re.search(r'^\w[^\n;]*\b'+name+r'\s*\([^;]*?\)\s*\n\{', text, re.M)
    return text[match.start():text.index('\n}', match.end())+2]+'\n'

source=r'''
#include <universal/q_shared.h>
#include <ui_mp/ui_mp.h>
#include <client_mp/client_mp.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#undef static_assert
static_assert(offsetof(UIServerBrowserStatus,displayServers)==1132);
static_assert(offsetof(UIServerBrowserStatus,numDisplayServers)==81132);
static_assert(offsetof(UIServerBrowserStatus,motd)==81176);
static_assert(sizeof(UIServerBrowserStatus)==82200);
static_assert(sizeof(mapInfo)==(sizeof(void*)==8?184:160));
static_assert(offsetof(mapInfo,levelShot)==(sizeof(void*)==8?168:152));
sharedUiInfo_t sharedUiInfo;
dvar_t testDvar={};
const dvar_t *ui_netSource=&testDvar,*ui_netGameType=&testDvar;
void MyAssertHandler(const char *file,int line,int,const char *fmt,...){fprintf(stderr,"%s:%d ",file,line);va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);abort();}
int LAN_CompareServers(int,int,int,unsigned int a,unsigned int b){return (a>b)-(a<b);}
int Com_sprintf(char *dst,unsigned int size,const char *fmt,...){va_list ap;va_start(ap,fmt);int n=vsnprintf(dst,size,fmt,ap);va_end(ap);return n;}
char *va(const char *fmt,...){static char buf[4096];va_list ap;va_start(ap,fmt);vsnprintf(buf,sizeof(buf),fmt,ap);va_end(ap);return buf;}
int I_stricmp(const char*a,const char*b){return _stricmp(a,b);}
void I_strncpyz(char*d,const char*s,int n){if(n>0){snprintf(d,n,"%s",s);}}
char *UI_SafeTranslateString(const char*s){return (char*)s;}
Material *Material_RegisterHandle(const char*s,int){return (Material*)s;}
void UI_SortServerStatusInfo(serverStatusInfo_t*){}
int LAN_GetServerStatus(char*,char *text,int n){if(text){snprintf(text,n,"\\sv_hostname\\test\\mapname\\mp_test\\\\10 20 player");}return 1;}
'''
for name in ['UI_ServersQsortCompare','UI_ServersSort','UI_InsertServerIntoDisplayList','UI_RemoveServerFromDisplayList','UI_BinaryServerInsertion','UI_MapCountByGameType','UI_GetMapDisplayName','UI_GetLevelShot','UI_GetServerStatusInfo','UI_ReplaceConversions','UI_ReplaceConversionString','UI_ReplaceConversionInt']:
    source+=extract('ui_main_mp.cpp',name)
source+=r'''
int main()
{
    UIServerBrowserStatus *b=&sharedUiInfo.serverStatus;
    b->currentServer=-1;b->numPlayersOnServers=0x12345678;
    for(int i=0;i<20000;++i){UI_InsertServerIntoDisplayList(i,i);}
    assert(b->numDisplayServers==20000&&b->numPlayersOnServers==0x12345678&&b->displayServers[19999]==19999);
    UI_InsertServerIntoDisplayList(20000,20000);UI_InsertServerIntoDisplayList(1,-1);
    assert(b->numDisplayServers==20000&&b->numPlayersOnServers==0x12345678);
    UI_RemoveServerFromDisplayList(10000);assert(b->numDisplayServers==19999&&b->displayServers[10000]==10001);
    UI_BinaryServerInsertion(10000);assert(b->numDisplayServers==20000);
    for(int i=0;i<20000;++i){assert(b->displayServers[i]==i);}
    sharedUiInfo.mapCount=128;
    for(int i=0;i<128;++i){mapInfo *m=&sharedUiInfo.mapList[i];m->mapName="display";m->mapLoadName="mp_test";m->imageName="loadscreen";m->typeBits=(int)(1u<<31);}
    testDvar.current.integer=31;assert(UI_MapCountByGameType()==128&&sharedUiInfo.mapList[127].active);
    assert(!strcmp(UI_GetMapDisplayName("mp_test"),"display")&&UI_GetLevelShot(127)==(Material*)sharedUiInfo.mapList[127].imageName);
    testDvar.current.integer=32;assert(UI_MapCountByGameType()==0);
    serverStatusInfo_t status={};assert(UI_GetServerStatusInfo((char*)"127.0.0.1",&status));
    assert(status.numLines==6&&!strcmp(status.lines[0][3],"127.0.0.1")&&!strcmp(status.lines[5][3],"player"));
    assert(!strcmp(UI_ReplaceConversionString((char*)"&&1",(char*)"100%"),"100%"));
    char output[8];ConversionArguments args={};args.argCount=1;args.args[0]="123456789";
    UI_ReplaceConversions((char*)"&&1",&args,output,sizeof(output));assert(!strcmp(output,"1234567"));
    UI_ReplaceConversions((char*)"&&0",&args,output,sizeof(output));assert(!strcmp(output,"&&0"));
    puts("PASS: 20000-server capacity/insert/remove/sort, map pointers/type bit 31, native status rows, bounded substitutions");
}
'''
test = out/'regression.cpp'
test.write_text(source)
probe = out/'regression-diagnostic.h'
probe.write_text('#define static_assert(...)\n')
for arch in ['x86','x64']:
    directory = out/arch
    directory.mkdir(exist_ok=True)
    args = ['/nologo','/O2','/std:c++20','/EHsc','/DRELEASE_ASSERTS','/DWIN32','/DKISAK_MP',
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
