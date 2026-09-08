"""Exercise production sound initialization and native pointer traversal."""
from pathlib import Path
import os
import re
import subprocess

root = Path.cwd()
out = Path(os.environ['TEMP'])/'kiwi-x64-epic/sound'
out.mkdir(parents=True, exist_ok=True)
vc = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
version = '10.0.26100.0'
includes = [root/'src', root/'deps', root/'deps/msslib', root/'build/_deps/tracy-src/public', vc/'include']
includes += [sdk/'Include'/version/n for n in ['ucrt', 'shared', 'um']]
includes += [Path('C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/include')]

def extract(file, name):
    text = (root/'src/sound'/file).read_text()
    match = re.search(r'^\w[^\n;]*\b'+name+r'\s*\(', text, re.M)
    return text[match.start():text.index('\n}', match.end())+2]+'\n'

source = r'''
#include <universal/q_shared.h>
#include <sound/snd_local.h>
#include <universal/com_files.h>
#include <universal/memfile.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#undef static_assert
static_assert(sizeof(LoadedSound)==(sizeof(void*)==8?64:44));
static_assert(sizeof(SndCurve)==(sizeof(void*)==8?80:72));
static_assert(sizeof(snd_alias_t)==(sizeof(void*)==8?128:92));
static_assert(sizeof(snd_alias_list_t)==(sizeof(void*)==8?24:12));
snd_local_t g_snd;
static dvar_t fade;
const dvar_t *snd_levelFadeTime=&fade;
static bool freeChannels[53];
void MyAssertHandler(const char*,int,int,const char*,...){abort();}
void Com_PrintError(int,const char*,...){}
void Com_Error(errorParm_t,const char*,...){throw 1;}
int I_stricmp(const char *a,const char *b){return _stricmp(a,b);}
bool SND_Is2DChannelFree(int i){return freeChannels[i];}
bool SND_Is3DChannelFree(int i){return freeChannels[i];}
bool SND_IsStreamChannelFree(int i){return freeChannels[i];}
void SND_StopSounds(snd_stopsounds_arg_t){}
static unsigned char allocation[128];
unsigned char *Hunk_Alloc(int n,const char*,int){assert(n==sizeof(LoadedSound));memset(allocation+n,0xa5,16);return allocation;}
void SND_SetData(MssSoundCOD4 *s,void *p){s->data=(byte*)p;}
unsigned int FS_FOpenFileReadStream(const char*,int *h){*h=37;return 12;}
void FS_FCloseFile(int h){assert(h==37);}
static byte saved[128];static int used,readPos;
void MemFile_WriteData(MemoryFile*,int n,const void *p){assert(used+n<=128);memcpy(saved+used,p,n);used+=n;}
void MemFile_ReadData(MemoryFile*,int n,byte *p){assert(readPos+n<=used);memcpy(p,saved+readPos,n);readPos+=n;}
static snd_alias_t alias;
static dvar_t fastFile;
const dvar_t *useFastFile=&fastFile;
uint *MSS_Alloc_FastFile(int){return (uint*)&alias;}
byte *MSS_Alloc_LoadObj(uint,uint){return (byte*)&g_snd;}
void SND_SaveSoundAlias(const snd_alias_t *p,MemoryFile*){assert(p==&alias);unsigned int token=0x11223344;MemFile_WriteData(NULL,4,&token);}
snd_alias_t *SND_RestoreSoundAlias(MemoryFile*){unsigned int token;MemFile_ReadData(NULL,4,(byte*)&token);assert(token==0x11223344);return &alias;}
'''
for file,names in {
 'snd_mss.cpp':['MSS_FileOpenCallback','MSS_FileCloseCallback','MSS_Alloc'],
 'snd_driver_load_obj.cpp':['SND_LoadFromBuffer'],
 'snd.cpp':['SND_FindPlaybackId','SND_MapInit','SND_SaveLengthNotifyInfo','SND_RestoreLengthNotifyInfo','SND_RestoreEventually'],
}.items():
    for name in names:
        if name=='SND_LoadFromBuffer':
            text=(root/'src/sound'/file).read_text();text=text[text.index('#else'):]
            match=re.search(r'^LoadedSound[^\n]*SND_LoadFromBuffer',text,re.M)
            source+=text[match.start():text.index('\n}',match.end())+2]+'\n'
        else:
            source+=extract(file,name)
source+=r'''
int main()
{
    UINTa handle=~(UINTa)0;assert(MSS_FileOpenCallback("dummy",&handle)&&handle==37);MSS_FileCloseCallback(handle);
    fastFile.current.enabled=true;assert(MSS_Alloc(1,22050)==(byte*)&alias);fastFile.current.enabled=false;assert(MSS_Alloc(1,22050)==(byte*)&g_snd);
    unsigned char wav[48]={'R','I','F','F',40,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,0x22,0x56,0,0,0x44,0xac,0,0,2,0,16,0,'d','a','t','a',4,0,0,0,0,0,0xff,0x7f};
    LoadedSound *sound=SND_LoadFromBuffer(wav,"pcm");
    assert(sound&&sound->sound.info.rate==22050&&sound->sound.info.data_len==4&&sound->sound.data==wav+44);
    for(int i=0;i<16;i++){assert(allocation[sizeof(LoadedSound)+i]==0xa5);}
    g_snd.Initialized2d=true;g_snd.max_2D_channels=8;g_snd.max_3D_channels=32;g_snd.max_stream_channels=13;
    alias.aliasName="match";
    for(int i=0;i<53;i++){g_snd.chaninfo[i].sndEnt=42;g_snd.chaninfo[i].playbackId=100+i;freeChannels[i]=true;}
    g_snd.chaninfo[52].alias1=&alias;freeChannels[52]=false;
    assert(SND_FindPlaybackId(42,"match")==152);
    assert(SND_FindPlaybackId(43,"match")==SND_PLAYBACKID_NOTPLAYED);
    g_snd.channelvol=&g_snd.channelVolGroups[0];g_snd.entchannel_count=3;fade.current.integer=250;
    for(int i=0;i<3;i++){g_snd.channelvol->channelvol[i].goalvolume=1;}
    SND_MapInit();for(int i=0;i<3;i++){assert(g_snd.channelvol->channelvol[i].goalrate==1.0f/250);}
    sndLengthNotifyInfo info={},restored;info.count=4;
    info.id[0]=info.id[1]=info.id[2]=SndLengthNotify_Script;info.id[3]=SndLengthNotify_Subtitle;
    info.data[0]=0;info.data[1]=(void*)(uintptr_t)0xf0123456;info.data[2]=(void*)(uintptr_t)123;info.data[3]=&alias;
    MemoryFile mem={};SND_SaveLengthNotifyInfo(&info,&mem);assert(used==24);
    memset(&restored,0xee,sizeof(restored));SND_RestoreLengthNotifyInfo(&mem,&restored);
    for(int i=0;i<4;i++){assert(restored.data[i]==info.data[i]);}
    byte segment[16]={16,0,0,0};mem.buffer=segment;mem.bufferSize=16;mem.bytesUsed=4;mem.compress=true;
    SND_RestoreEventually(&mem);assert(g_snd.restore.size==16&&g_snd.restore.compress&&!memcmp(g_snd.restore.buffer,segment,16));
    *(int*)segment=-1;mem.bytesUsed=4;bool rejected=false;try{SND_RestoreEventually(&mem);}catch(int){rejected=true;}assert(rejected&&!g_snd.restore.size);
    puts("PASS: native Miles file handle and actual WAV parser, loaded-sound canary/layouts, all-channel lookup, fade rates, fixed notification archive, restore bounds");
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
    args += [str(root/('deps/msslib/x64/mss64.lib' if arch=='x64' else 'deps/msslib/mss32.lib'))]
    result = subprocess.run([str(vc/f'bin/Hostx64/{arch}/cl.exe')]+args,capture_output=True,text=True)
    (directory/'build.txt').write_text(result.stdout+result.stderr)
    assert result.returncode==0,result.stdout+result.stderr
    env=os.environ.copy();env['PATH']=str(root/('deps/msslib/x64' if arch=='x64' else 'deps/msslib/dlls'))+';'+env['PATH']
    result = subprocess.run([str(directory/'regression.exe')],capture_output=True,text=True,env=env)
    (directory/'run.txt').write_text(result.stdout+result.stderr)
    assert result.returncode==0,result.stdout+result.stderr
    print(arch,result.stdout.strip())
