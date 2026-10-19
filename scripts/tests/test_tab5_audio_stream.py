"""Exercise stereo timing, authenticated proxy URLs and MP3 Range preroll."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/qdtech/tab5'
HELIX = ROOT / 'managed_components/chmorgan__esp-libhelix-mp3/libhelix-mp3'


@unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
class AudioStreamTests(unittest.TestCase):
    def test_stereo_keeps_channels_and_fractional_output_time(self):
        source = r'''
#include <cassert>
#include <vector>
#include "tab5_audio_stream.h"
int main() {
    std::vector<int16_t> input(2304), output(7000);
    for (int i=0;i<1152;++i) { input[2*i]=12000; input[2*i+1]=-21000; }
    uint64_t phase=0, total=0;
    for(int packet=0;packet<10000;++packet) {
        int count=tab5_audio_stream::ResampleStereo(input.data(),2304,2,44100,24000,
                                                   output.data(),output.size(),&phase);
        assert(count==626 || count==627); total+=count;
        for(int i=0;i<count;++i) assert(output[2*i]==12000 && output[2*i+1]==-21000);
    }
    const double expected=10000.0*1152*24000/44100;
    assert(total>=expected && total<expected+1); // no accumulated per-frame truncation
    assert(tab5_audio_stream::ResampleStereo(input.data(),2304,2,44100,24000,output.data(),1)==0);
    assert(tab5_audio_stream::ResampleStereo(input.data(),2303,2,44100,24000,output.data(),7000)==0);
    assert(tab5_audio_stream::ResampleStereo(input.data(),2304,3,44100,24000,output.data(),7000)==0);
    for(int i=0;i<1152;++i) input[i]=int16_t(i*17-10000);
    int count=tab5_audio_stream::ResampleStereo(input.data(),1152,1,24000,24000,output.data(),7000);
    assert(count==1152);
    for(int i=0;i<count;++i) assert(output[2*i]==input[i] && output[2*i+1]==input[i]);
}
'''
        self.run_cpp(source)

    def test_proxy_range_validation_and_bookmark_identity(self):
        self.run_cpp(r'''
#include <cassert>
#include "tab5_audio_stream.h"
int main() {
    using namespace tab5_audio_stream;
    assert(RelayUrl("https://relay.example/inbox/test-token", "", "http://cdn.example/a.mp3?x=1&y=2",false)==
      "https://relay.example/podcast/test-token/stream96?url=http%3A%2F%2Fcdn.example%2Fa.mp3%3Fx%3D1%26y%3D2&live=0");
    assert(RelayUrl("","nas:8787","http://radio.example/live",true).find("/tab5/podcast/stream96?")!=std::string::npos);
    for (auto u : {"file:///x", "", "https://relay.example/podcast/x/stream96?live=1"})
        assert(RelayUrl("https://relay.example/inbox/token","",u,false).empty());
    assert(RelayUrl("bad-route","","http://cdn.example/x",false).empty());
    auto range=ParseRange("bytes 1000-9999/10000");
    assert(range.valid && range.first==1000 && range.last==9999 && range.total==10000);
    for(auto h:{"bytes */10000","bytes 0-10/10","bytes 10-9/100","bytes 0-1/2147483648",
                "bytes 18446744073709551616-20/30","bytes 0-1/2junk","0-1/2"}) assert(!ParseRange(h).valid);
    ResumePoint point;
    assert(!point.Matches("song-a",7));
    point.Capture("song-a",7,20000,1300);
    assert(point.Matches("song-a",7) && point.Start()==0);
    assert(!point.Matches("song-a",8) && !point.Matches("song-b",7));
    point.Capture("song-a",7,300,26);assert(point.Start()==0);
}
''')

    def run_cpp(self, source):
        with tempfile.TemporaryDirectory() as directory:
            cpp, exe = Path(directory)/'test.cc', Path(directory)/'test'
            cpp.write_text(source)
            result = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-I', str(BOARD), str(cpp), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(HELIX.exists() and shutil.which('ffmpeg') and shutil.which('cc'),
                         'Helix sources, ffmpeg and C compiler required')
    def test_real_helix_preroll_matches_continuous_audio_after_range_resume(self):
        # Keep vendor files untouched. Only the CPU-assembly intrinsics in a
        # temporary host copy are replaced with their portable integer forms.
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            lib = temp/'helix'; shutil.copytree(HELIX, lib)
            (lib/'real/assembly.h').write_text(r'''
#pragma once
#include <stdint.h>
typedef int64_t Word64;
static inline int MULSHIFT32(int a,int b) {return (int)(((int64_t)a*b)>>32);}
static inline int FASTABS(int x) {int s=x>>31;return (x^s)-s;}
static inline int CLZ(int x) {return x?__builtin_clz((unsigned)x):32;}
static inline Word64 MADD64(Word64 s,int a,int b) {return s+(Word64)a*b;}
static inline Word64 SHL64(Word64 x,int n) {return x<<n;}
static inline Word64 SAR64(Word64 x,int n) {return x>>n;}
''')
            mp3 = temp/'stereo.mp3'
            subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i',
                'aevalsrc=0.2*sin(2*PI*440*t)|0.2*sin(2*PI*880*t):s=44100:d=12',
                '-c:a','libmp3lame','-b:a','96k','-write_xing','0','-id3v2_version','0',str(mp3)],
                check=True, capture_output=True)
            objects=[]
            for i, src in enumerate([*sorted(lib.glob('*.c')), *sorted((lib/'real').glob('*.c'))]):
                obj=temp/f'{i}.o'; objects.append(str(obj))
                result=subprocess.run(['cc','-O2','-fwrapv','-I',str(lib/'pub'),'-I',str(lib/'real'),
                                       '-c',str(src),'-o',str(obj)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
            cpp=temp/'preroll.cc';exe=temp/'preroll'
            cpp.write_text(r'''
#include <cassert>
#include <cstdint>
#include <fstream>
#include <cstdio>
#include <iterator>
#include <vector>
#include "mp3dec.h"
#include "tab5_audio_stream.h"
struct Frame {size_t end;std::vector<short> pcm;};
int main(int, char** argv) {
    std::ifstream file(argv[1],std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)),{});
    auto decode=[&](size_t start,size_t skip) {
        auto decoder=MP3InitDecoder();assert(decoder);
        auto* ptr=data.data()+start;int left=int(data.size()-start);short pcm[2304];
        std::vector<Frame> out;int errors=0;
        while(left>4) {
            int offset=MP3FindSyncWord(ptr,left);if(offset<0)break;
            ptr+=offset;left-=offset;
            int err=MP3Decode(decoder,&ptr,&left,pcm,0);
            if(err==ERR_MP3_INDATA_UNDERFLOW || err==ERR_MP3_MAINDATA_UNDERFLOW)continue;
            if(err) {fprintf(stderr,"start=%zu consumed=%zu err=%d left=%d\n",start,data.size()-left,err,left);assert(++errors<4);if(left){++ptr;--left;}continue;}
            errors=0;size_t consumed=data.size()-left;
            if(consumed<=skip)continue;
            MP3FrameInfo info{};MP3GetLastFrameInfo(decoder,&info);
            assert(info.bitrate==96000 && info.samprate==44100 && info.nChans==2);
            out.push_back({consumed,std::vector<short>(pcm,pcm+info.outputSamps)});
        }
        MP3FreeDecoder(decoder);return out;
    };
    const auto continuous=decode(0,0);assert(continuous.size()>400);
    for(size_t at:{1,2,5,15,65,113,375}) {
        tab5_audio_stream::ResumePoint point;
        for(size_t i=0;i<=at;++i) point.Capture("same-source",7,continuous[i].end,0);
        const auto resumed=decode(point.Start(),point.byte);
        assert(resumed.size()==continuous.size()-at-1);
        for(size_t i=0;i<resumed.size();++i) {
            assert(resumed[i].end==continuous[at+1+i].end);
            assert(resumed[i].pcm==continuous[at+1+i].pcm);
        }
    }
}
''')
            result=subprocess.run(['c++','-O2','-std=c++17','-I',str(BOARD),'-I',str(lib/'pub'),
                                   str(cpp),*objects,'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe),str(mp3)],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stderr)
