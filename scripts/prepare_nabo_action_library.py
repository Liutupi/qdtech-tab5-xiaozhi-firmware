#!/usr/bin/env python3
"""Package user-selected RGBA reviews and original SD clips for Tab5.

Requires numpy, Pillow, scipy, a system liblz4 (or --lz4-library), and ffmpeg.
Writes a separate lively-v1 directory; never touches a mounted SD card.
"""
import argparse
import ctypes
import ctypes.util
import hashlib
import json
import struct
import subprocess
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage

WIDTH, HEIGHT, FPS = 320, 349, 25
IDS = ['A06', 'A09', 'A16', 'E04', 'E05', 'E06', 'E09', 'E14', 'P01', 'P02',
       'A03R1', 'A05R1', 'E12R1', 'E03R2']


def read_original(path):
    data = path.read_bytes()
    assert data[:8] == b'NABOSD1\0' and zlib.crc32(data[:60]) == struct.unpack_from('<I', data, 60)[0]
    count, duration = struct.unpack_from('<II', data, 20)
    index = data[64:64 + count * 32]
    assert zlib.crc32(index) == struct.unpack_from('<I', data, 48)[0]
    assert duration == count * 40
    for n in range(count):
        at, off, size, crc, x, y, w, h = struct.unpack_from('<IIIIHHHH', index, n * 32)
        assert (x, y, w, h, at, size) == (0, 0, 320, 412, n * 40, 320 * 412 * 3)
        payload = data[off:off + size]
        assert len(payload) == size and zlib.crc32(payload) == crc
        c = np.frombuffer(payload[:w*h*2], '<u2').reshape(h, w).astype(np.uint32)
        a = np.empty((h, w, 4), np.uint8)
        a[:, :, 0] = (c >> 11) * 255 // 31
        a[:, :, 1] = ((c >> 5) & 63) * 255 // 63
        a[:, :, 2] = (c & 31) * 255 // 31
        a[:, :, 3] = np.frombuffer(payload[w*h*2:], np.uint8).reshape(h, w)
        art = Image.new('RGBA', (512, 558))
        art.alpha_composite(Image.fromarray(a).resize((434, 558), Image.Resampling.LANCZOS), (39, 0))
        s = .94
        yield art.transform((512, 558), Image.Transform.AFFINE,
                            (1/s, 0, 256*(1-1/s), 0, 1/s, 534*(1-1/s)), Image.Resampling.BICUBIC)


def clean(art):
    a = np.array(art)
    labels, count = ndimage.label(a[:, :, 3] > 8)
    if not count:
        raise ValueError('Empty character')
    areas = np.bincount(labels.ravel())
    areas[0] = 0
    main = labels == areas.argmax()
    # Preserve attached hair/edge alpha and nearby separated wisps. Remove only
    # detached green-screen floaters, never clip a hand to fit the window.
    near = ndimage.binary_dilation(main, iterations=2)
    keep = np.unique(labels[near & (labels != 0)])
    support = ndimage.binary_dilation(np.isin(labels, keep), iterations=2)
    a[~support] = 0
    a[a[:, :, 3] <= 8] = 0
    a[:, :, 3][a[:, :, 3] >= 250] = 255
    return Image.fromarray(a)


def blend(a, b, weight):
    a, b = np.array(a, dtype=np.float32)/255, np.array(b, dtype=np.float32)/255
    a[:, :, :3] *= a[:, :, 3:4]
    b[:, :, :3] *= b[:, :, 3:4]
    p = a * (1-weight) + b * weight
    rgb = np.divide(p[:, :, :3], p[:, :, 3:4], out=np.zeros_like(p[:, :, :3]), where=p[:, :, 3:4] > 0)
    return Image.fromarray(np.uint8(np.clip(np.dstack([rgb, p[:, :, 3:4]]), 0, 1)*255))


def encode(art):
    a = np.array(art.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS))
    a[a[:, :, 3] <= 3] = 0
    rgb = a[:, :, :3].astype(np.uint16)
    c = ((rgb[:, :, 0] >> 3) << 11) | ((rgb[:, :, 1] >> 2) << 5) | (rgb[:, :, 2] >> 3)
    return c.astype('<u2').tobytes() + a[:, :, 3].tobytes()


class Codec:
    def __init__(self, path):
        self.lib = ctypes.CDLL(path or ctypes.util.find_library('lz4'))
        self.lib.LZ4_compressBound.argtypes = [ctypes.c_int]
        self.lib.LZ4_compress_default.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        self.lib.LZ4_decompress_safe.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]

    def compress(self, raw):
        out = ctypes.create_string_buffer(self.lib.LZ4_compressBound(len(raw)))
        n = self.lib.LZ4_compress_default(raw, out, len(raw), len(out))
        assert n > 0
        compressed = out.raw[:n]
        check = ctypes.create_string_buffer(len(raw))
        assert self.lib.LZ4_decompress_safe(compressed, check, n, len(raw)) == len(raw)
        assert check.raw == raw
        return (compressed, 2) if n < len(raw) else (raw, 1)


def write_pack(path, arts, codec):
    payloads, rows, sizes, decoded_crcs = [], [], [], []
    offset = 64 + len(arts)*32
    for n, art in enumerate(arts):
        raw = encode(art)
        payload, mode = codec.compress(raw)
        pad = (-offset) % 128
        payloads.append(bytes(pad) + payload)
        offset += pad
        rows.append(struct.pack('<IIIIHHHHBBHI', n*40, offset, len(payload), zlib.crc32(payload),
                                0, 0, WIDTH, HEIGHT, mode, 1, 0, 0))
        offset += len(payload)
        sizes.append(len(payload))
        decoded_crcs.append(zlib.crc32(raw))
    index = b''.join(rows)
    header = bytearray(64)
    header[:8] = b'NABOSD1\0'
    struct.pack_into('<HHHHHH', header, 8, 3, 64, WIDTH, HEIGHT, WIDTH, HEIGHT)
    struct.pack_into('<IIIIIIIII', header, 20, len(arts), len(arts)*40, 64,
                     64+len(arts)*32, WIDTH*HEIGHT*3, 3, offset, zlib.crc32(index), 0)
    struct.pack_into('<I', header, 60, zlib.crc32(header[:60]))
    data = bytes(header) + index + b''.join(payloads)
    assert len(data) == offset
    path.write_bytes(data)
    return dict(file=path.name, frames=len(arts), duration_ms=len(arts)*40, bytes=len(data),
                sha256=hashlib.sha256(data).hexdigest(), encoded_mean_bytes=sum(sizes)/len(sizes),
                encoded_max_bytes=max(sizes), read_MB_s_at_25fps=sum(sizes)/len(sizes)*25/1e6,
                first_decoded_crc=decoded_crcs[0], last_decoded_crc=decoded_crcs[-1],
                all_lz4_roundtrips_verified=True)


def preview_page(art):
    page = Image.new('RGBA', (512, 558), '#10283d')
    d = ImageDraw.Draw(page)
    d.rounded_rectangle((-2, 340, 503, 561), 28, fill='#193b56')
    d.rectangle((12, 354, 489, 538), fill='#183550')
    body = Image.new('RGBA', page.size)
    body.paste(art.crop((12, 354, 490, 539)), (12, 354))
    page.alpha_composite(body)
    d = ImageDraw.Draw(page)
    d.rounded_rectangle((-2, 340, 503, 363), 12, fill='#285272')
    head = art.copy(); a = np.array(head); a[423:, :, 3] = 0
    page.alpha_composite(Image.fromarray(a))
    d = ImageDraw.Draw(page)
    for x in [-2, 490]: d.rounded_rectangle((x, 360, x+14, 546), 7, fill='#234a64')
    d.rounded_rectangle((-2, 534, 503, 544), 3, fill='#38617a')
    d.rectangle((4, 534, 497, 535), fill='#7796aa')
    d.rounded_rectangle((7, 544, 494, 557), 8, fill='#193b56')
    return page.convert('RGB')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--batch', type=Path, required=True)
    p.add_argument('--lab', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--lz4-library')
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if list(args.output.glob('*.nab')):
        raise RuntimeError('Use a new output directory to preserve existing packages')
    codec = Codec(args.lz4_library)
    job = json.loads((args.batch/'job.json').read_text())
    selected = {x['id']: x for x in job['items'] if x.get('preview_eligible') and x.get('user_selected')}
    assert set(IDS) <= selected.keys(), 'All 14 candidates must be explicitly selected'
    original_paths = {'idle': args.lab/'resources/replacement-v3/nabo-idle-new-source-25fps.nab',
                      **{key: args.lab/f'resources/three-state/{key}.nab' for key in ['transition', 'sleep', 'work']}}
    originals, reports = {}, []
    for key, path in original_paths.items():
        originals[key] = [clean(x) for x in read_original(path)]
        r = write_pack(args.output/f'{key}.nab', originals[key], codec)
        r.update(id=key, source=str(path), source_sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        reports.append(r)
        print(json.dumps(r, ensure_ascii=False), flush=True)
    neutral = originals['idle'][0]
    font = ImageFont.truetype('/System/Library/Fonts/STHeiti Medium.ttc', 22)
    sheet = Image.new('RGB', (1536, 5*335), '#0b1725')
    video = args.output.parent/'NABO-all-14-integrated-preview.mp4'
    enc = subprocess.Popen(['ffmpeg', '-v', 'error', '-n', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                            '-s', '512x608', '-r', '25', '-i', 'pipe:0', '-an', '-c:v', 'libx264',
                            '-preset', 'fast', '-crf', '20', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', str(video)], stdin=subprocess.PIPE)
    for i, key in enumerate(IDS):
        item = selected[key]
        assert hashlib.sha256(Path(item['source_path']).read_bytes()).hexdigest() == item['source_sha256']
        review_path = args.batch/key/'review-v2/review.json'
        review = json.loads(review_path.read_text())
        assert review['inside_B_sides'] and not review['source_edge_frames']
        with np.load(review['alpha_cache']) as cache:
            arts = [clean(Image.fromarray(a)) for a in cache['frames']]
        for n in range(6):
            t = n/5; weight = t*t*(3-2*t)
            for frame in [n, len(arts)-1-n]: arts[frame] = blend(neutral, arts[frame], weight)
        arts[0] = neutral.copy(); arts[-1] = neutral.copy()
        r = write_pack(args.output/f'{key}.nab', arts, codec)
        assert r['first_decoded_crc'] == reports[0]['first_decoded_crc'] == r['last_decoded_crc']
        r.update(id=key, name=item['name'], source_sha256=item['source_sha256'], user_selected=True,
                 endpoint_settle_ms=240, full_character_fit=.94, source_cache=str(review['alpha_cache']))
        reports.append(r)
        still = preview_page(arts[len(arts)//2]).resize((256, 279), Image.Resampling.LANCZOS)
        x, y = (i % 6)*256, (i//6)*335
        sheet.paste(still, (x, y)); ImageDraw.Draw(sheet).text((x+8, y+282), key+' '+item['name'].split('·')[0], font=font, fill='white')
        # Include an actual original idle loop between each full generated action.
        for art in originals['idle'] + arts:
            frame = Image.new('RGB', (512, 608), '#0b1725'); frame.paste(preview_page(art))
            ImageDraw.Draw(frame).text((12, 572), key+' '+item['name'].split('·')[0], font=font, fill='white')
            enc.stdin.write(frame.tobytes())
        print(json.dumps(r, ensure_ascii=False), flush=True)
    enc.stdin.close()
    assert enc.wait() == 0
    sheet.crop((0, 0, 1536, 3*335)).save(args.output.parent/'NABO-all-14-integrated-overview.jpg', quality=93)
    manifest = dict(format='NABOSD1 version3, independent LZ4 RGB565A8 frames', source_fps=25,
                    dimensions=[WIDTH, HEIGHT], display_dimensions=[512, 558], version='lively-v1',
                    device_directory='/sdcard/tab5/nabo/lively-v1', actions=reports,
                    total_bytes=sum(x['bytes'] for x in reports), selected_action_count=len(IDS),
                    original_state_count=4, preview=str(video), hardware_validation=False)
    (args.output/'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
