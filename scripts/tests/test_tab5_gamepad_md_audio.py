"""Real shared controller decoding and bounded MD audio mixing."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"

SMOKE = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include "tab5_gamepad_report.h"
#include "md_audio_mix.h"
using namespace tab5_gamepad;
int main() {
    uint8_t x[20] = {0, 20};
    assert(Parse(Layout::Xinput, x, sizeof(x)).nes == 0);
    const uint8_t dpad[] = {1,2,4,8};
    const uint8_t nesdir[] = {kNesBtnUp,kNesBtnDown,kNesBtnLeft,kNesBtnRight};
    for (int i=0;i<4;++i) {
        x[2]=dpad[i]; auto r=Parse(Layout::Xinput,x,sizeof(x));
        assert(r.nes==nesdir[i] && r.md==dpad[i]);
    }
    x[2]=0;
    x[3]=0x40; assert(Parse(Layout::Xinput,x,20).md==0x40); // west MD A
    x[3]=0x10; assert(Parse(Layout::Xinput,x,20).md==0x20); // south MD B
    x[3]=0x20; assert(Parse(Layout::Xinput,x,20).md==0x10); // east MD C
    x[3]=0; x[2]=0x30;
    assert(Parse(Layout::Xinput,x,20).nes==(kNesBtnStart|kNesBtnSelect));
    assert(Parse(Layout::Xinput,x,20).md==0x80);
    x[2]=0; x[6]=0x20;x[7]=0x4e;
    assert(Parse(Layout::Xinput,x,20).nes==kNesBtnRight);
    assert(Parse(Layout::Xinput,x,9).nes==0);
    uint8_t ds[10]={1,128,128,128,128,8,0};
    assert(Parse(Layout::Ds4,ds,10).nes==0);
    ds[5]=0x18;assert(Parse(Layout::Ds4,ds,10).md==0x40);
    ds[5]=0x28;assert(Parse(Layout::Ds4,ds,10).md==0x20);
    ds[5]=0x48;assert(Parse(Layout::Ds4,ds,10).md==0x10);
    ds[5]=8;ds[6]=0x30;
    assert(Parse(Layout::Ds4,ds,10).nes==(kNesBtnStart|kNesBtnSelect));
    ds[6]=0; assert(Parse(Layout::Ds4,ds,10).md==0);
    // Neutral 8-bit HID axes must not also be reinterpreted as 16-bit sticks.
    uint8_t hid[8]={0,0,15,128,128,0,0,0};
    assert(Parse(Layout::Hid,hid,8).nes==0);
    hid[0]=4;assert(Parse(Layout::Hid,hid,8).md==0x40);
    hid[0]=1;assert(Parse(Layout::Hid,hid,8).md==0x20);
    hid[0]=2;assert(Parse(Layout::Hid,hid,8).md==0x10);
    hid[0]=0;hid[2]=1;
    assert(Parse(Layout::Hid,hid,8).md==0x09);
    assert(Parse(Layout::Hid,nullptr,0).nes==0);
    // PAL has more than the original 889 source samples: both modes stay bounded.
    int16_t fm[1200], psg[1200], guarded[962];
    for (int i=0;i<1200;++i) {fm[i]=20000;psg[i]=10000;}
    for (int n : {400,480}) {
        guarded[0]=123;guarded[2*n+1]=-123;
        md_mix_audio(fm, n==400?888:1060, psg, n==400?888:1060, guarded+1,n);
        assert(guarded[0]==123 && guarded[2*n+1]==-123);
        for (int i=1;i<=2*n;++i) assert(guarded[i]==15000);
    }
    md_mix_audio(nullptr,0,nullptr,0,guarded,400);
    for(int i=0;i<800;++i) assert(guarded[i]==0);
    int16_t ramp[]={0,1000};
    assert(md_sample_at(ramp,2,0,4)==0 && md_sample_at(ramp,2,1,4)==500);
    assert(md_sample_at(ramp,2,3,4)==1000);
}
'''

class GamepadMdAudioTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_controller_layouts_and_audio_bounds(self):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "smoke.cc"
            cpp.write_text(SMOKE)
            binary = Path(directory) / "smoke"
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(BOARD),
                            "-I", str(ROOT / "updater/main"), str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    unittest.main()
