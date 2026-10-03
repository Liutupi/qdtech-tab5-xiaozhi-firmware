import importlib.util
from pathlib import Path
import random
import subprocess
import tempfile
import unittest

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('nabo_image_codec', ROOT / 'scripts/nabo_image_codec.py')
codec = importlib.util.module_from_spec(spec)
spec.loader.exec_module(codec)


class NaboImageCodecTest(unittest.TestCase):
    def test_lossless_blocks_with_lvgl_decoder(self):
        vendor = ROOT / 'managed_components/lvgl__lvgl'
        if not (vendor / 'src/libs/lz4/lz4.c').exists():
            self.skipTest('LVGL component must be installed to check its native decoder')
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            executable = directory / 'decode'
            subprocess.run(['cc', '-std=c99', '-DLV_CONF_SKIP', '-DLV_USE_LZ4=1',
                            '-DLV_USE_LZ4_INTERNAL=1', '-I', str(vendor),
                            str(vendor / 'src/libs/lz4/lz4.c'),
                            str(ROOT / 'scripts/tests/nabo_lz4_decode.c'), '-o', str(executable)], check=True)
            cases = [b'', b'x', bytes(range(20)), b'a' * 200000,
                     bytes(range(256)) * 300, random.Random(123).randbytes(150000)]
            # Include actual generated bodies, not just synthetic byte streams.
            import re
            source = (ROOT / 'main/boards/qdtech/tab5/nabo_assets.c').read_text()
            for name in ('think', 'wink', 'encourage', 'curious', 'comfort'):
                match = re.search(r'static const uint8_t nabo_' + name +
                                  r'_torso_data\[\].*?= \{(.*?)\};', source, re.S)
                data = bytes(int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]{2})', match[1]))
                encoded, decoded = directory / 'body.block', directory / 'body.raw'
                import struct
                method, size, out_size = struct.unpack_from('<III', data)
                self.assertEqual(method, 2)
                self.assertEqual(size, len(data) - 12)
                encoded.write_bytes(data[12:])
                subprocess.run([str(executable), str(encoded), str(decoded), str(out_size)], check=True)
                cases.append(decoded.read_bytes())
            for index, expected in enumerate(cases):
                encoded, decoded = directory / f'{index}.block', directory / f'{index}.raw'
                encoded.write_bytes(codec.lz4_block(expected))
                subprocess.run([str(executable), str(encoded), str(decoded), str(len(expected))], check=True)
                self.assertEqual(decoded.read_bytes(), expected)

    def test_rgb565_planes_keep_alpha(self):
        image = Image.new('RGBA', (2, 1))
        image.putdata([(248, 252, 248, 0), (0, 0, 0, 193)])
        self.assertEqual(codec.rgb565a8(image), b'\xff\xff\x00\x00\x00\xc1')
