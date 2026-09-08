"""Exercise production universal initialization and native pointer traversal."""
from pathlib import Path
import os
import re
import subprocess

root = Path.cwd()
out = Path(os.environ['TEMP'])/'kiwi-x64-epic/win32'
out.mkdir(parents=True, exist_ok=True)
vc = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
version = '10.0.26100.0'
includes = [root/'src', root/'deps', root/'deps/msslib', root/'build/_deps/tracy-src/public', vc/'include']
includes += [sdk/'Include'/version/n for n in ['ucrt', 'shared', 'um']]
includes += [Path('C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/include')]


def extract(file,name):
    text=(root/'src/win32'/file).read_text()
    m=re.search(r'^\w[^\n;]*\b'+name+r'\s*\([^;]*?\)\s*\n\{',text,re.M)
    return text[m.start():text.index('\n}',m.end())+2]+'\n'
source=r'''
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <win32/win_local.h>
#include <win32/win_storage.h>
#include <Windows.h>
#include <tlhelp32.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <type_traits>
#undef static_assert
static_assert(sizeof(StatsFile)==0x211C);
static_assert(sizeof(sysEvent_t)==(sizeof(void*)==8?32:24));
void MyAssertHandler(const char *file,int line,int,const char *fmt,...){fprintf(stderr,"%s:%d ",file,line);abort();}
void Com_Error(errorParm_t,const char*,...){throw 1;}
void Com_Printf(int,const char*,...){}
void Com_PrintWarning(int,const char*,...){}
const char *NET_ErrorString(){return "test socket";}
void NET_Sleep(int ms){Sleep(ms);}
int I_stricmp(const char *a,const char *b){return _stricmp(a,b);}
qboolean Sys_StringToSockaddr(const char *s,sockaddr *address){memset(address,0,sizeof(sockaddr));sockaddr_in *ip=(sockaddr_in*)address;ip->sin_family=AF_INET;ip->sin_addr.s_addr=inet_addr(s);return ip->sin_addr.s_addr!=INADDR_NONE;}
void Sys_Print(const char*){}
int Com_sprintf(char *out,unsigned int size,const char *format,...){va_list ap;va_start(ap,format);int n=vsnprintf(out,size,format,ap);va_end(ap);return n;}
'''
for name in ['NET_IPSocket','NET_Select','NET_TCPIPSocket']:
    source+=extract('win_net.cpp',name)
for name in ['Sys_GetPhysicalCpuCount','Sys_DetectCpuVendorAndName']:
    source+=extract('win_configure.cpp',name)
source+=extract('win_main.cpp','Sys_IsGameProcess')
text=(root/'src/win32/win_syscon.cpp').read_text()
a=text.index('struct WinConData');source+=text[a:text.index('};',a)+2]+'\nstatic WinConData s_wcd;\n'
source+=extract('win_syscon.cpp','InputLineWndProc')
source+=r'''
static SOCKET lastSocket;
static byte sentData[10000];static int sentBytes,sendCalls;
static bool failSend;
int TestSend(SOCKET socket,const char *buffer,int len,int){lastSocket=socket;++sendCalls;if(failSend){return 0;}if(len>2){len=2;}memcpy(sentData+sentBytes,buffer,len);sentBytes+=len;return len;}
#define send TestSend
void Sys_DebugSocketError(const char*){throw 2;}
char *va(const char*,...){return (char*)"test";}
static uint32_t g_debugReadBytes,g_debugReadBytesSent;
static SOCKET ip_debugSocket[2];
'''
for name in ['Sys_DebugSendAll','Sys_SendDebugReadBytesInternal','Sys_SendDebugReadBytes']:
    source+=extract('win_net_debug.cpp',name)
source+=r'''
static int mixerChecks;
UINT TestDeviceCount(){return 1;}
MMRESULT TestMixerOpen(HMIXER *m,UINT,DWORD_PTR,DWORD_PTR,DWORD){*m=(HMIXER)(uintptr_t)0x1234;return 0;}
MMRESULT TestMixerClose(HMIXER){++mixerChecks;return 0;}
MMRESULT TestMixerLine(HMIXEROBJ, MIXERLINEA *line,DWORD){assert(line->cbStruct==sizeof(MIXERLINEA));line->cConnections=1;strcpy(line->szName,"Mic");++mixerChecks;return 0;}
MMRESULT TestMixerControls(HMIXEROBJ,MIXERLINECONTROLSA *controls,DWORD){assert(controls->cbStruct==sizeof(MIXERLINECONTROLSA)&&controls->cbmxctrl==sizeof(MIXERCONTROLA));controls->pamxctrl->dwControlID=42;++mixerChecks;return 0;}
MMRESULT TestMixerDetails(HMIXEROBJ,MIXERCONTROLDETAILS *details,DWORD){assert(details->cbStruct==sizeof(MIXERCONTROLDETAILS)&&details->dwControlID==42);((MIXERCONTROLDETAILS_UNSIGNED*)details->paDetails)->dwValue=12345;++mixerChecks;return 0;}
#define waveInGetNumDevs TestDeviceCount
#define mixerGetNumDevs TestDeviceCount
#define mixerOpen TestMixerOpen
#define mixerClose TestMixerClose
#define mixerGetLineInfoA TestMixerLine
#define mixerGetLineControlsA TestMixerControls
#define mixerGetControlDetailsA TestMixerDetails
'''
source+=extract('win_voice.cpp','mixerGetRecordLevel')
source+=r'''
#undef send
static WPARAM forwardedW;static LPARAM forwardedL;
LRESULT CALLBACK ForwardedProc(HWND,UINT,WPARAM w,LPARAM l){forwardedW=w;forwardedL=l;return (LRESULT)w;}
int main()
{
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    WSADATA wsa;assert(!WSAStartup(MAKEWORD(2,2),&wsa));
    SOCKET udp=NET_IPSocket("127.0.0.1",0);assert(udp!=INVALID_SOCKET);
    sockaddr_in address={};int size=sizeof(sockaddr_in);assert(!getsockname(udp,(sockaddr*)&address,&size));
    assert(sendto(udp,"hello",5,0,(sockaddr*)&address,size)==5);
    char buf[8]={};int received=-1;
    for(int i=0;i<100&&received<0;++i){received=recv(udp,buf,sizeof(buf),0);if(received<0){Sleep(1);}}
    assert(received==5&&!memcmp(buf,"hello",5));closesocket(udp);
    SOCKET listener=NET_TCPIPSocket("127.0.0.1",0,0);assert(listener!=INVALID_SOCKET);assert(!listen(listener,1));
    size=sizeof(sockaddr_in);assert(!getsockname(listener,(sockaddr*)&address,&size));
    SOCKET client=NET_TCPIPSocket("127.0.0.1",ntohs(address.sin_port),1);assert(client!=INVALID_SOCKET);
    SOCKET peer=accept(listener,NULL,NULL);assert(peer!=INVALID_SOCKET);
    assert(send(client,"tcp",3,0)==3);received=-1;
    for(int i=0;i<100&&received<0;++i){received=recv(peer,buf,sizeof(buf),0);if(received<0){Sleep(1);}}
    assert(received==3&&!memcmp(buf,"tcp",3));closesocket(peer);closesocket(client);closesocket(listener);
    WSACleanup();
    assert(mixerGetRecordLevel((char*)"Mic")==12345&&mixerChecks==5);
    SysInfo info={};info.logicalCpuCount=64;Sys_DetectCpuVendorAndName(info.cpuVendor,info.cpuName);Sys_GetPhysicalCpuCount(&info);
    assert(strlen(info.cpuVendor)==12&&strlen(info.cpuName)>0&&info.physicalCpuCount>0);
    assert(Sys_IsGameProcess(GetCurrentProcessId()));
    uintptr_t native=sizeof(void*)==8?UINT64_C(0x1234567887654321):0x87654321u;
    s_wcd.SysInputLineWndProc=ForwardedProc;
    assert(InputLineWndProc(NULL,WM_APP,(WPARAM)native,(LPARAM)native)==(LRESULT)native);
    assert(forwardedW==native&&(uintptr_t)forwardedL==native);
    ip_debugSocket[1]=(SOCKET)native;Sys_SendDebugReadBytes(8192);assert(lastSocket==native&&sentBytes==4&&sendCalls==2);
    uint32_t count;memcpy(&count,sentData,sizeof(uint32_t));assert(count==8192&&g_debugReadBytesSent==8192);
    g_debugReadBytes=UINT32_MAX-2;g_debugReadBytesSent=g_debugReadBytes;Sys_SendDebugReadBytes(5);assert(g_debugReadBytes==2);
    failSend=true;bool failed=false;try{Sys_DebugSendAll(ip_debugSocket[1],"x",1);}catch(int){failed=true;}assert(failed);
    puts("PASS: real UDP/TCP loopback, CPU topology/CPUID, module enumeration, native mixer structures and callback forwarding, partial debugger sends and 32-bit acknowledgement wrap");
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
    args += ['/Fe'+str(directory/'regression.exe'),'/Fo'+str(directory/'regression.obj'),str(test),'/link','ws2_32.lib','user32.lib']
    args += ['/LIBPATH:'+str(vc/f'lib/{arch}')]
    args += ['/LIBPATH:'+str(sdk/'Lib'/version/n/arch) for n in ['ucrt','um']]
    result = subprocess.run([str(vc/f'bin/Hostx64/{arch}/cl.exe')]+args,capture_output=True,text=True)
    (directory/'build.txt').write_text(result.stdout+result.stderr)
    assert result.returncode==0,str(result.returncode)+result.stdout+result.stderr
    result = subprocess.run([str(directory/'regression.exe')],capture_output=True,text=True)
    (directory/'run.txt').write_text(result.stdout+result.stderr)
    assert result.returncode==0,result.stdout+result.stderr
    print(arch,result.stdout.strip())
