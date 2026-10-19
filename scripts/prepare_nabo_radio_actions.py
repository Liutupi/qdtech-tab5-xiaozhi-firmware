#!/usr/bin/env python3
"""Package the NABO FM radio-DJ clips for the Tab5 radio page.

Input is the 480x480 green-screen batch from ai-passport-nabo-xiaozhi-radio
(design/nabo-radio/art/video/batch1/source). Output is one NABOSD1 version-3
pack per action (independent LZ4 RGB565A8 frames, each cropped to its own
bounding box) plus manifest.json, for /sdcard/tab5/nabo/radio-v1 on the card.
Requires numpy, Pillow, ffmpeg and a system liblz4. Never touches a mounted card.
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

SRC = 480
DESK = 420  # the desk edge sits at source row 420; the UI draws the desk
# 94% keeps the largest frame (RGB565A8) inside a 571 KB slot of the home scene pool.
WIDTH, HEIGHT = 451, 395
SOURCE_FPS = 8
LOOP_MS, ONE_SHOT_MS = 125, 62  # one-shot gestures play twice as fast (about 3 s)
VERSION = 'radio-v1'
# Must match ActionName() in main/boards/qdtech/tab5/nabo_radio_mood.h.
ACTIONS = [
    ('idle_off', 'idle_off.mp4', True), ('wave', 'wave.mp4', False),
    ('phones_on', 'phones_on_v2.mp4', False), ('phones_off', 'phones_off.mp4', False),
    ('tune', 'tune.mp4', True), ('groove', 'groove.mp4', True), ('sing', 'sing.mp4', True),
    ('listen', 'listen.mp4', True), ('sip', 'sip.mp4', False), ('love', 'love.mp4', False),
    ('oops', 'oops.mp4', False), ('doze', 'doze.mp4', True), ('search', 'search.mp4', True),
]


def frames(path):
    cmd = ['ffmpeg', '-v', 'error', '-i', str(path), '-vf', f'fps={SOURCE_FPS}',
           '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-']
    raw = subprocess.run(cmd, check=True, capture_output=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(-1, SRC, SRC, 3)


def key(rgb):
    """Green screen -> RGBA. Same keyer as the AI Passport build, kept at full size."""
    f = rgb.astype(np.float32)
    r, g, b = f[..., 0], f[..., 1], f[..., 2]
    green = g - np.maximum(r, b)
    alpha = 1.0 - np.clip((green - 40.0) / 110.0, 0, 1)
    g2 = np.where(green > 0, np.minimum(g, np.maximum(r, b) + 8), g)
    near = ~(alpha > 0.98)
    for _ in range(4):
        near = near | np.roll(near, 1, 0) | np.roll(near, -1, 0) | np.roll(near, 1, 1) | np.roll(near, -1, 1)
    g2 = np.where(near, np.minimum(g2, (r + b) / 2 + 10), g2)
    yellow_green = (g2 > (r + b) / 2 + 25) & (g2 > r * 0.92)
    g2 = np.where(yellow_green, np.minimum(g2, r * 0.85), g2)
    out = np.clip(np.stack([r, g2, b, alpha * 255], -1)[:DESK], 0, 255).astype(np.uint8)
    out = scale(out)
    out[out[..., 3] <= 3] = 0
    out[..., 3][out[..., 3] >= 250] = 255
    return out


def scale(rgba):
    """Resize with premultiplied alpha so no green fringe bleeds into the edges."""
    from PIL import Image
    a = rgba[..., 3:4].astype(np.float32) / 255
    planes = [*(rgba[..., c].astype(np.float32) * a[..., 0] for c in range(3)), a[..., 0] * 255]
    small = [np.asarray(Image.fromarray(q, 'F').resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS))
             for q in planes]
    alpha = np.clip(small[3], 0, 255)
    rgb = [np.divide(q, alpha / 255, out=np.zeros_like(q), where=alpha > 0) for q in small[:3]]
    return np.clip(np.stack([*rgb, alpha], -1), 0, 255).astype(np.uint8)


def crop(rgba):
    """Bounding box of visible pixels, padded by 2 px and kept inside the canvas."""
    ys, xs = np.nonzero(rgba[..., 3])
    if not len(xs):
        return 0, 0, 1, 1
    x0, y0 = max(0, xs.min() - 2), max(0, ys.min() - 2)
    height, width = rgba.shape[:2]
    x1, y1 = min(width, xs.max() + 3), min(height, ys.max() + 3)
    return int(x0), int(y0), int(x1 - x0), int(y1 - y0)


def encode(rgba):
    rgb = rgba[..., :3].astype(np.uint16)
    c = ((rgb[..., 0] >> 3) << 11) | ((rgb[..., 1] >> 2) << 5) | (rgb[..., 2] >> 3)
    return c.astype('<u2').tobytes() + rgba[..., 3].tobytes()


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


def write_pack(path, arts, interval_ms, codec):
    height, width = arts[0].shape[:2]
    payloads, rows, sizes = [], [], []
    offset = 64 + len(arts) * 32
    max_w = max_h = 1
    for n, art in enumerate(arts):
        x, y, w, h = crop(art)
        max_w, max_h = max(max_w, w), max(max_h, h)
        payload, mode = codec.compress(encode(art[y:y + h, x:x + w]))
        pad = (-offset) % 128
        payloads.append(bytes(pad) + payload)
        offset += pad
        rows.append(struct.pack('<IIIIHHHHBBHI', n * interval_ms, offset, len(payload),
                                zlib.crc32(payload), x, y, w, h, mode, 1, 0, 0))
        offset += len(payload)
        sizes.append(len(payload))
    index = b''.join(rows)
    header = bytearray(64)
    header[:8] = b'NABOSD1\0'
    struct.pack_into('<HHHHHH', header, 8, 3, 64, width, height, max_w, max_h)
    duration = len(arts) * interval_ms
    struct.pack_into('<IIIIIIIII', header, 20, len(arts), duration, 64, 64 + len(arts) * 32,
                     max_w * max_h * 3, 3, offset, zlib.crc32(index), 0)
    struct.pack_into('<I', header, 60, zlib.crc32(header[:60]))
    data = bytes(header) + index + b''.join(payloads)
    assert len(data) == offset
    path.write_bytes(data)
    return dict(file=path.name, frames=len(arts), duration_ms=duration, bytes=len(data),
                max_frame=[max_w, max_h], sha256=hashlib.sha256(data).hexdigest(),
                encoded_mean_bytes=round(sum(sizes) / len(sizes)), encoded_max_bytes=max(sizes),
                read_MB_s=round(sum(sizes) / len(sizes) * 1000 / interval_ms / 1e6, 2))


def prop(path, width):
    """A still prop (microphone, mug) trimmed to its pixels and scaled to width."""
    from PIL import Image
    im = Image.open(path).convert('RGBA')
    im = im.crop(im.getbbox())
    im = im.resize((width, round(im.height * width / im.width)), Image.Resampling.LANCZOS)
    a = np.array(im)
    a[a[..., 3] <= 3] = 0
    return a


def write_still(path, rgba, codec):
    """One-frame pack whose canvas is the prop itself."""
    return write_pack(path, [rgba], 1000, codec)


BOARD = Path(__file__).resolve().parent.parent / 'main/boards/qdtech/tab5'
FONT_SIZE = 56
FONT_UI = 'NABO FM 网络电台 播放中 已暂停 连接中 收藏 点歌 下一首 下一台 上一台 全部 其他 还没有台'


def font_symbols(catalogs):
    """Every non-ASCII character a station name can use, plus printable ASCII."""
    text = FONT_UI + (BOARD / 'radio_service.cc').read_text(encoding='utf-8')
    text += (BOARD / 'radio.json.sample').read_text(encoding='utf-8')
    for catalog in catalogs:
        text += catalog.read_text(encoding='utf-8')
    chars = {c for c in text if ord(c) > 0x2e7f and c.isprintable()}
    return ''.join(chr(c) for c in range(0x20, 0x7f)) + ''.join(sorted(chars))


def write_font(ttf, output, catalogs):
    symbols = font_symbols(catalogs)
    subprocess.run(['npx', '--yes', 'lv_font_conv@1.5.3', '--font', str(ttf), '--size', str(FONT_SIZE),
                    '--bpp', '4', '--format', 'bin', '--symbols', symbols, '-o', str(output)],
                   check=True)
    data = output.read_bytes()
    return dict(file=output.name, size_px=FONT_SIZE, glyphs=len(symbols), bytes=len(data),
                sha256=hashlib.sha256(data).hexdigest())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--clips', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--props', type=Path, help='art/props/source with mic_source.png and mug_source.png')
    p.add_argument('--font', type=Path, help='LXGWWenKai-Regular.ttf for the station-name font')
    p.add_argument('--catalog', type=Path, action='append', default=[],
                   help='extra station lists whose names the font must cover')
    p.add_argument('--lz4-library')
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if list(args.output.glob('*.nab')):
        raise RuntimeError('Use a new output directory to preserve existing packages')
    codec = Codec(args.lz4_library)
    reports = []
    for action, clip, loop in ACTIONS:
        source = args.clips / clip
        arts = [key(f) for f in frames(source)]
        r = write_pack(args.output / f'{action}.nab', arts, LOOP_MS if loop else ONE_SHOT_MS, codec)
        r.update(id=action, loop=loop, source=clip,
                 source_sha256=hashlib.sha256(source.read_bytes()).hexdigest())
        reports.append(r)
        print(json.dumps(r, ensure_ascii=False), flush=True)
    extras = []
    if args.props:
        for name, width in [('mic', 112), ('mug', 64)]:
            r = write_still(args.output / f'{name}.nab', prop(args.props / f'{name}_source.png', width), codec)
            r.update(id=name)
            extras.append(r)
    if args.font:
        extras.append(write_font(args.font, args.output / f'wenkai_{FONT_SIZE}.bin', args.catalog))
    for r in extras:
        print(json.dumps(r, ensure_ascii=False), flush=True)
    manifest = dict(format='NABOSD1 version3, independent LZ4 RGB565A8 frames, per-frame crop',
                    dimensions=[WIDTH, HEIGHT], version=VERSION,
                    device_directory=f'/sdcard/tab5/nabo/{VERSION}', actions=reports, extras=extras,
                    total_bytes=sum(x['bytes'] for x in reports + extras), hardware_validation=False)
    (args.output / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
