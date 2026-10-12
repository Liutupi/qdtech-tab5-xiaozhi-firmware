#!/usr/bin/env python3
"""Cross-check every decoded frame against liblz4 and preview actual SD pixels."""
import argparse
import ctypes
import hashlib
import json
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from prepare_nabo_action_library import Codec, IDS, preview_page


def frames(path, codec):
    data = path.read_bytes()
    assert zlib.crc32(data[:60]) == struct.unpack_from('<I', data, 60)[0]
    count, duration = struct.unpack_from('<II', data, 20)
    w, h = struct.unpack_from('<HH', data, 12)
    assert (w, h, duration) == (320, 349, count*40)
    index = data[64:64+count*32]
    assert zlib.crc32(index) == struct.unpack_from('<I', data, 48)[0]
    for i in range(count):
        at, off, size, crc = struct.unpack_from('<IIII', index, i*32)
        assert at == i*40 and off % 128 == 0
        payload = data[off:off+size]
        assert len(payload) == size and zlib.crc32(payload) == crc
        if index[i*32+24] == 2:
            out = ctypes.create_string_buffer(w*h*3)
            assert codec.lib.LZ4_decompress_safe(payload, out, size, len(out)) == len(out)
            raw = out.raw
        else:
            raw = payload
        c = np.frombuffer(raw[:w*h*2], '<u2').reshape(h, w).astype(np.uint32)
        a = np.empty((h, w, 4), np.uint8)
        a[:, :, 0] = (c >> 11)*255//31
        a[:, :, 1] = ((c >> 5) & 63)*255//63
        a[:, :, 2] = (c & 31)*255//31
        a[:, :, 3] = np.frombuffer(raw[w*h*2:], np.uint8).reshape(h, w)
        # Exact integer sampling used by WideComposer, rather than a high-res
        # source review that hides the device's RGB565 and sampling appearance.
        sx = np.arange(512)*w//512; sy = np.arange(558)*h//558
        yield raw, Image.fromarray(a[sy[:, None], sx[None, :]])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    p.add_argument('--lz4-library')
    args = p.parse_args()
    manifest = json.loads((args.directory/'manifest.json').read_text())
    root = Path(__file__).resolve().parents[1]
    paths = [args.directory/x['file'] for x in manifest['actions']]
    with tempfile.TemporaryDirectory() as directory:
        binary = Path(directory)/'check'
        subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I', str(root/'main/boards/qdtech/tab5'),
                        str(root/'scripts/tests/tab5_sd_lz4_smoke.cc'), '-o', str(binary)], check=True)
        result = subprocess.check_output([str(binary), *map(str, paths)], text=True)
    runtime_crc = {line.split()[1]: int(line.split()[2], 16) for line in result.splitlines() if line.startswith('DECODED ')}
    codec = Codec(args.lz4_library)
    font = ImageFont.truetype('/System/Library/Fonts/STHeiti Medium.ttc', 22)
    video = args.directory.parent/'NABO-SD-pixels-preview.mp4'
    encoder = subprocess.Popen(['ffmpeg', '-v', 'error', '-n', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                                '-s', '512x608', '-r', '25', '-i', 'pipe:0', '-an', '-c:v', 'libx264',
                                '-preset', 'fast', '-crf', '18', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', str(video)], stdin=subprocess.PIPE)
    reports, idle = [], []
    sheet = Image.new('RGB', (1536, 1005), '#0b1725')
    thumb_index = 0
    neutral_crc = manifest['actions'][0]['first_decoded_crc']
    for entry, path in zip(manifest['actions'], paths):
        assert hashlib.sha256(path.read_bytes()).hexdigest() == entry['sha256']
        crc, first, last, outside = 0, None, None, []
        images = []
        for n, (raw, art) in enumerate(frames(path, codec)):
            crc = zlib.crc32(raw, crc)
            current = zlib.crc32(raw)
            first = current if first is None else first; last = current
            a = np.asarray(art)
            yy, xx = np.where(a[:, :, 3] > 160)
            # Before the sill hides the lower torso, all hands/head remain
            # within the accepted B side posts. Never crop an arm to pass.
            if entry['id'] in IDS and (xx.min() < 13 or xx.max() >= 490): outside.append(n)
            images.append(art)
        assert crc == runtime_crc[str(path)], f"Firmware/liblz4 mismatch: {path}"
        if entry['id'] == 'idle': idle = images
        if entry['id'] in IDS:
            assert first == last == neutral_crc
            assert not outside, f"B side-post collision in {path}: {outside}"
            for art in idle + images:
                frame = Image.new('RGB', (512, 608), '#0b1725')
                frame.paste(preview_page(art))
                ImageDraw.Draw(frame).text((12, 572), entry['id']+' '+entry['name'].split('·')[0], font=font, fill='white')
                encoder.stdin.write(frame.tobytes())
            x, y = thumb_index % 6*256, thumb_index//6*335
            sheet.paste(preview_page(images[len(images)//2]).resize((256, 279)), (x, y))
            ImageDraw.Draw(sheet).text((x+8, y+282), entry['id']+' '+entry['name'].split('·')[0], font=font, fill='white')
            thumb_index += 1
        reports.append(dict(id=entry['id'], frames=len(images), decoded_crc32=f'{crc:08x}',
                            firmware_matches_lib_lz4=True, outside_B_frames=outside))
    encoder.stdin.close(); assert encoder.wait() == 0
    subprocess.run(['ffmpeg', '-v', 'error', '-i', str(video), '-f', 'null', '-'], check=True)
    overview = args.directory.parent/'NABO-SD-pixels-overview.jpg'
    sheet.save(overview, quality=93)
    report = dict(frames=sum(x['frames'] for x in reports), clips=reports, selected_actions=14,
                  exact_neutral_endpoints=True, all_B_bounds_pass=True, preview=str(video),
                  overview=str(overview), audio_tracks=0, hardware_validated=False)
    (args.directory.parent/'asset-validation.json').write_text(json.dumps(report, ensure_ascii=False, indent=2))
    print(json.dumps({k:v for k,v in report.items() if k != 'clips'}, ensure_ascii=False))


if __name__ == '__main__': main()
