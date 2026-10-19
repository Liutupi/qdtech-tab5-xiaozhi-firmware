"""Favorite-song codec, bounded inputs, atomic writes and reboot recovery."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
BOARD = Path(__file__).resolve().parents[2] / 'main/boards/qdtech/tab5'

@unittest.skipUnless(shutil.which('c++'), 'C++ host compiler required')
class SongFavoritesTests(unittest.TestCase):
    def test_storage_and_interrupted_commit_recovery(self):
        source=r'''
#include <cassert>
#include <fstream>
#include "nabo_song_favorites.h"
using namespace nabo_song_favorites;
int main(int argc,char**argv) {
 assert(argc==2);const std::string path=std::string(argv[1])+"/favorites.bin";
 List old{{"歌一","歌手","12345678901234567890"},{"歌一","另一歌手"}},next{{"新歌","新歌手"}},read;
 Bytes encoded;assert(Encode(old,encoded));assert(Decode(encoded.data(),encoded.size(),read));
 assert(read[0].song_id=="12345678901234567890" && read[1].song_id.empty());
 assert(!Valid({"歌","歌手","123x"}) && !Valid({"歌","歌手","-1"}));
 assert(!Valid({"歌","歌手","123456789012345678901"}));
 assert(read.size()==2 && Same(read[0],old[0]) && !Same(read[0],read[1]));
 const auto previous=read;
 for(size_t i=0;i<encoded.size();++i) {auto corrupt=encoded;corrupt[i]^=0x80;assert(!Decode(corrupt.data(),corrupt.size(),read));assert(read.size()==previous.size());}
 for(size_t i=0;i<encoded.size();++i) assert(!Decode(encoded.data(),i,read));
 List duplicate=old;duplicate.push_back(old[0]);assert(!Encode(duplicate,encoded));
 assert(!Valid({"", "artist"}));assert(!Valid({std::string(257,'a').c_str(),"artist"}));
 assert(!TextValid(std::string("a\0b",3),256,false));
 assert(!TextValid("\xed\xa0\x80",256,false));assert(!TextValid("\xc0\xaf",256,false));
 assert(TextValid("复杂的中文、English & / 𠮷",256,false));
 List full;for(unsigned i=0;i<kMaxSongs;++i) full.push_back({tab5_podcast::ToStr("歌"+std::to_string(i)),"歌手"});
 assert(Encode(full,encoded));assert(encoded.size()<=kMaxFileBytes);full.push_back({"超出","歌手"});assert(!Encode(full,encoded));
 assert(SaveFile(path,old));assert(LoadFile(path,read) && read.size()==2);
 assert(SaveFile(path,next));assert(LoadFile(path,read) && read.size()==1 && Same(read[0],next[0]));
 // Interrupted after the old file moved away: prefer the fully written new temporary file.
 assert(SaveFile(path,old));assert(Encode(next,encoded));
 {std::ofstream out(path+".tmp",std::ios::binary);out.write(reinterpret_cast<const char*>(encoded.data()),encoded.size());}
 assert(std::rename(path.c_str(),(path+".bak").c_str())==0);
 assert(LoadFile(path,read) && read.size()==1 && Same(read[0],next[0]));
 // A torn temp file falls back to the complete previous file.
 {std::ofstream out(path+".tmp",std::ios::binary);out<<"torn";}
 assert(LoadFile(path,read) && read.size()==2);
 assert(SaveFile(path,next));assert(LoadFile(path,read) && read.size()==1);
 assert(!SaveFile(path+"/unwritable",old));assert(LoadFile(path,read) && read.size()==1);
 assert(!SaveFile(path,duplicate));assert(LoadFile(path,read) && read.size()==1);
 // Corrupt data never silently turns into an empty collection.
 {std::ofstream out(path,std::ios::binary);out<<"bad";}
 assert(!LoadFile(path,read));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'test.cc').write_text(source)
            compiled=subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(BOARD),str(root/'test.cc'),'-o',str(root/'test')],capture_output=True,text=True)
            self.assertEqual(compiled.returncode,0,compiled.stderr)
            subprocess.run([str(root/'test'),str(root)],check=True,capture_output=True,text=True)
