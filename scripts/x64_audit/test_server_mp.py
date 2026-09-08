"""Exercise production server_mp initialization and native pointer traversal."""
from pathlib import Path
import os
import re
import subprocess

root = Path.cwd()
out = Path(os.environ['TEMP'])/'kiwi-x64-epic/server_mp'
out.mkdir(parents=True, exist_ok=True)
vc = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
version = '10.0.26100.0'
includes = [root/'src', root/'deps', root/'deps/msslib', root/'build/_deps/tracy-src/public', vc/'include']
includes += [sdk/'Include'/version/n for n in ['ucrt', 'shared', 'um']]
includes += [Path('C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/include')]

def extract(file, name):
    text = (root/'src/server_mp'/file).read_text()
    match = re.search(r'^\w[^\n;]*\b'+name+r'\s*\(', text, re.M)
    return text[match.start():text.index('\n}', match.end())+2]+'\n'

source = r'''
#include <universal/q_shared.h>
#include <server_mp/server_mp.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#undef static_assert
serverStatic_t svs;
server_t sv;
static dvar_t maximum;
const dvar_t *sv_maxclients=&maximum;
static size_t allocated;
static byte *allocation;
static int commands;
void MyAssertHandler(const char*,int,int,const char*,...){}
void Com_Memset(void *p,int value,size_t size){memset(p,value,size);}
void SV_SendServerCommand(client_t*,svscmd_type,const char*,...){commands++;}
void SV_BoundMaxClients(int minimum){assert(minimum==3);maximum.current.integer=4;}
uint *Hunk_AllocateTempMemory(int size,const char*){allocated=size;allocation=(byte*)malloc(size+16);memset(allocation+size,0xa5,16);return (uint*)allocation;}
void Hunk_FreeTempMemory(char *p){assert(p==(char*)allocation);for(int i=0;i<16;i++){assert(allocation[allocated+i]==0xa5);}free(allocation);}
'''
for file,names in {
 'sv_client_mp.cpp':['SV_SetClientStat','SV_GetClientStat'],
 'sv_init_mp.cpp':['SV_ChangeMaxClients'],
 'sv_voice_mp.cpp':['SV_QueueVoicePacket'],
}.items():
    for name in names:
        source+=extract(file,name)
source+=r'''
int main()
{
    maximum.current.integer=3;
    client_t *c=&svs.clients[2];
    c->header.state=CS_CONNECTED;c->statPacketsReceived=127;
    if(sizeof(void*)==8){assert((uintptr_t)c>UINT32_MAX);}
    memset(c->voicePackets,0x5a,sizeof(c->voicePackets));
    for(int i=0;i<3498;i++){
        unsigned int value=i<2000?(i%255)+1:0xf0120000u+i;
        SV_SetClientStat(2,i,value);assert((unsigned int)SV_GetClientStat(2,i)==value);
        SV_SetClientStat(2,i,value);
    }
    assert(commands==3498);
    for(size_t i=0;i<sizeof(c->voicePackets);i++){assert(((byte*)c->voicePackets)[i]==0x5a);}
    assert(c->stats[0]==0&&c->stats[7996]==0);
    assert(!SV_GetClientStat(2,-1));SV_SetClientStat(2,3498,1);assert(commands==3498);
    c->downloadBlocks[7]=(byte*)c;c->gentity=(gentity_s*)c;
    c->stats[8191]=0xd4;
    SV_ChangeMaxClients();
    assert(allocated==3*sizeof(client_t));
    assert(c->gentity==(gentity_s*)c&&c->downloadBlocks[7]==(byte*)c&&c->stats[8191]==0xd4);
    for(size_t i=0;i<sizeof(client_t);i++){assert(((byte*)&svs.clients[1])[i]==0);assert(((byte*)&svs.clients[3])[i]==0);}
    VoicePacket_t packet={};packet.dataSize=256;memset(packet.data,0x39,256);
    for(int i=0;i<41;i++){SV_QueueVoicePacket(1,2,&packet);}
    assert(c->voicePacketCount==40&&c->voicePackets[39].talker==1&&c->voicePackets[39].data[255]==0x39);
    c->voicePacketCount=0;packet.dataSize=257;SV_QueueVoicePacket(1,2,&packet);assert(!c->voicePacketCount);
    packet.dataSize=-1;SV_QueueVoicePacket(1,2,&packet);assert(!c->voicePacketCount);
    static_assert(sizeof(playerState_s)==0x2f64);
    static_assert(offsetof(playerState_s,clientNum)==55*4);
    static_assert(offsetof(playerState_s,origin)==7*4);
    static_assert(offsetof(playerState_s,viewHeightCurrent)==70*4);
    static_assert(offsetof(playerState_s,viewangles)+sizeof(float)==67*4);
    static_assert(offsetof(playerState_s,leanf)==23*4);
    puts("PASS: all 3498 stats, voice canaries/capacity, native client resize/pointer preservation, player-state offsets");
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
