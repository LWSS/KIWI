"""Build/run real Speex codec and extracted voice boundary functions on x86/x64."""
from pathlib import Path
import concurrent.futures
import os
import re
import subprocess

root = Path.cwd()
out = Path(os.environ['TEMP'])/'kiwi-x64-epic/groupvoice'
out.mkdir(parents=True, exist_ok=True)
vc = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
version = '10.0.26100.0'
includes = [root/'src', root/'deps', vc/'include']
includes += [sdk/'Include'/version/n for n in ['ucrt', 'shared', 'um']]

def extract(file, name):
    source = (root/'src/groupvoice'/file).read_text()
    match = re.search(r'^\w[^\n;]*\b'+name+r'\s*\(', source, re.M)
    return source[match.start():source.index('\n}', match.end())+2]

source = r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef unsigned int uint;
#include <groupvoice/directsound.h>
#include <groupvoice/speex/stack_alloc.h>
#include <speex/speex.h>
#define ARRAY_COUNT(x) (sizeof(x)/sizeof(x[0]))
void *g_decoder;
int g_decode_frame_size;
SpeexBits decodeBits;
bool g_recording_initialized=true;
dsound_sample_t s_recordingSamples[65];
dsound_sample_t *s_recordingSamplePtr=s_recordingSamples;
'''
source += extract('decode.cpp', 'Decode_Sample')+'\n'
source += extract('record_dsound.cpp', 'DSOUNDRecord_NewSample')+'\n'
source += r'''
int main()
{
    setbuf(stdout,NULL);
    char scratch[4096];
    for (int offset=0;offset<32;offset++)
    {
        char *stack=scratch+offset;
        void **p=PUSHS(stack,void *);
        assert(((uintptr_t)p%sizeof(void *))==0);
        double *d=PUSH(stack,3,double);
        assert(((uintptr_t)d%sizeof(double))==0);
        d[2]=1.25;
    }
    for (int i=0;i<65;i++)
    {
        assert(DSOUNDRecord_NewSample()==s_recordingSamples+i);
    }
    assert(!DSOUNDRecord_NewSample());
    assert(s_recordingSamplePtr==s_recordingSamples+65);
    const SpeexMode *modes[]={&speex_nb_mode,&speex_wb_mode,&speex_uwb_mode};
    for (int mode=0;mode<3;mode++)
    {
        printf("bandwidth %d\n",mode);
        void *encoder=speex_encoder_init(modes[mode]);
        g_decoder=speex_decoder_init(modes[mode]);
        assert(encoder&&g_decoder);
        speex_decoder_ctl(g_decoder,SPEEX_GET_FRAME_SIZE,&g_decode_frame_size);
        SpeexBits encoded;
        speex_bits_init(&encoded);
        speex_bits_init(&decodeBits);
        for (int frame=0;frame<20;frame++)
        {
            int16_t input[640];
            int16_t decoded[642];
            char packet[4096];
            for (int i=0;i<640;i++) { input[i]=(int16_t)((i%31-15)*200); }
            for (int i=0;i<642;i++) { decoded[i]=12345; }
            speex_bits_reset(&encoded);
            speex_encode_int(encoder,input,&encoded);
            int bytes=speex_bits_write(&encoded,packet,sizeof(packet));
            assert(bytes>0);
            assert(!Decode_Sample(packet,bytes,decoded+1,g_decode_frame_size-1));
            assert(decoded[1]==12345);
            assert(Decode_Sample(packet,bytes,decoded+1,g_decode_frame_size)==2*g_decode_frame_size);
            assert(decoded[0]==12345&&decoded[g_decode_frame_size+1]==12345);
            assert(!Decode_Sample(packet,-1,decoded+1,g_decode_frame_size));
        }
        speex_bits_destroy(&encoded);
        speex_bits_destroy(&decodeBits);
        speex_encoder_destroy(encoder);
        speex_decoder_destroy(g_decoder);
    }
    puts("PASS: native DirectSound layouts, scratch alignment, 65-slot capacity, 60 codec frames and decode canaries");
}
'''
test = out/'regression.cpp'
test.write_text(source)
for arch in ['x86', 'x64']:
    directory = out/arch
    directory.mkdir(exist_ok=True)
    compiler = str(vc/f'bin/Hostx64/{arch}/cl.exe')
    flags = ['/nologo', '/O2', '/D_CRT_SECURE_NO_WARNINGS']+['/I'+str(p) for p in includes]
    if os.environ.get('KIWI_TEST_ASAN'):
        flags += ['/fsanitize=address', '/Z7']
    files = sorted((root/'src/groupvoice/speex').glob('*.c'))+[test]
    def compile_file(file):
        args = flags+(['/std:c++20', '/EHsc'] if file.suffix == '.cpp' else [])
        result = subprocess.run([compiler]+args+['/c', str(file), '/Fo'+str(directory/(file.stem+'.obj'))], capture_output=True, text=True)
        (directory/(file.stem+'.log')).write_text(result.stdout+result.stderr)
        if result.returncode:
            raise RuntimeError(result.stdout+result.stderr)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(compile_file, files))
    command = [str(vc/'bin/Hostx64/x64/link.exe'), '/nologo', '/OUT:'+str(directory/'regression.exe')]
    command += [str(directory/(p.stem+'.obj')) for p in files]
    command += ['/LIBPATH:'+str(vc/f'lib/{arch}')]
    command += ['/LIBPATH:'+str(sdk/'Lib'/version/n/arch) for n in ['ucrt', 'um']]
    if os.environ.get('KIWI_TEST_ASAN'):
        command += ['/INFERASANLIBS', '/DEBUG']
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout+result.stderr
    environment = dict(os.environ)
    environment['PATH'] = str(vc/f'bin/Hostx64/{arch}')+';'+environment['PATH']
    result = subprocess.run([str(directory/'regression.exe')], capture_output=True, text=True, env=environment)
    (directory/'run.txt').write_text(result.stdout+result.stderr)
    assert result.returncode == 0, result.stdout+result.stderr
    print(arch, result.stdout.strip())
