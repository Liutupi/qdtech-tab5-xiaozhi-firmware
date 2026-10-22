#!/usr/bin/env python3
"""Append explicitly selected reviewed clips; preserve every existing pack byte."""
import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path

import numpy as np
from PIL import Image

from prepare_nabo_action_library import Codec, blend, clean, encode, write_pack
from validate_nabo_action_library import frames


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def decode_neutral(raw):
    w, h = 320, 349
    pixels = np.frombuffer(raw[:w*h*2], '<u2').reshape(h, w).astype(np.uint32)
    rgba = np.empty((h, w, 4), np.uint8)
    rgba[:, :, 0] = (pixels >> 11)*255//31
    rgba[:, :, 1] = ((pixels >> 5) & 63)*255//63
    rgba[:, :, 2] = (pixels & 31)*255//31
    rgba[:, :, 3] = np.frombuffer(raw[w*h*2:], np.uint8).reshape(h, w)
    image = Image.fromarray(rgba)
    assert encode(image) == raw, 'Neutral RGB565 must round-trip exactly'
    return image


def fit_character(arts, review_fit):
    """One fixed whole-image transform; no source content may leave the canvas."""
    left, top, right, bottom = 512, 558, 0, 0
    for art in arts:
        ys, xs = np.where(np.asarray(art)[:, :, 3] > 8)
        left, top = min(left, int(xs.min())), min(top, int(ys.min()))
        right, bottom = max(right, int(xs.max())), max(bottom, int(ys.max()))
    ax, ay = 256, 534
    limits = [.94/review_fit]
    for extent, distance in [(ax-14, ax-left), (488-ax, right-ax),
                             (ay-1, ay-top), (556-ay, bottom-ay)]:
        if distance > 0:
            limits.append(extent/distance)
    factor = min(limits)
    if factor < .94/review_fit:
        factor *= .995  # Keep interpolation support inside the safe region.
    if abs(factor-1) > 1e-6:
        affine = (1/factor, 0, ax*(1-1/factor), 0, 1/factor, ay*(1-1/factor))
        arts = [art.transform((512, 558), Image.Transform.AFFINE, affine,
                              Image.Resampling.BICUBIC) for art in arts]
    for art in arts:
        ys, xs = np.where(np.asarray(art)[:, :, 3] > 160)
        assert xs.min() >= 13 and xs.max() < 490
        assert ys.min() > 0 and ys.max() < 558
    return arts, factor


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--selection', type=Path, required=True)
    parser.add_argument('--base-library', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--lz4-library')
    args = parser.parse_args()
    requested = json.loads(args.selection.read_text())['selected']
    assert requested and all(x['decision'] == 'keep' for x in requested)
    assert len({x['candidate_id'] for x in requested}) == len(requested)
    base = json.loads((args.base_library/'manifest.json').read_text())
    codec = Codec(args.lz4_library)
    args.output.mkdir(parents=True, exist_ok=True)
    if list(args.output.glob('*.nab')):
        raise ValueError('Use an empty output directory; existing packs are never overwritten')
    entries = []
    for entry in base['actions']:
        filename = entry['file']
        assert Path(filename).name == filename
        source = args.base_library/filename
        assert digest(source) == entry['sha256']
        shutil.copy2(source, args.output/filename)
        assert digest(args.output/filename) == entry['sha256']
        entries.append(dict(entry))
    raw, _ = next(frames(args.output/'idle.nab', codec))
    neutral = decode_neutral(raw)
    neutral_high = neutral.resize((512, 558), Image.Resampling.NEAREST)
    ids = {x['id'] for x in entries}
    added, deferred = [], []
    for item in requested:
        key = item['code']
        assert re.fullmatch(r'[A-Z][0-9]+(?:R[0-9]+)?', key) and key not in ids
        source = Path(item['source_path'])
        assert digest(source) == item['source_sha256']
        review = json.loads(Path(item['review_file']).read_text())
        assert review['source_sha256'] == item['source_sha256']
        if review['source_edge_frames'] or not review['inside_B_sides']:
            deferred.append({'candidate_id': item['candidate_id'], 'id': key,
                             'name': item['name'], 'reason': 'source_edge_clipping_or_frame_bounds',
                             'source_edge_frames': review['source_edge_frames']})
            continue
        with np.load(review['alpha_cache'], allow_pickle=False) as cache:
            arts = [clean(Image.fromarray(frame)) for frame in cache['frames']]
        assert len(arts) == review['decoded_frames'] and len(arts) >= 12
        assert all(art.size == (512, 558) for art in arts)
        arts, scale = fit_character(arts, review['fixed_whole_character_scale'])
        # Reuse the accepted v1 full-character fade, never joint/limb edits.
        for n in range(6):
            t = n/5
            weight = t*t*(3-2*t)
            for frame in [n, len(arts)-1-n]:
                arts[frame] = blend(neutral_high, arts[frame], weight)
        arts[0], arts[-1] = neutral.copy(), neutral.copy()
        report = write_pack(args.output/f'{key}.nab', arts, codec)
        assert report['first_decoded_crc'] == report['last_decoded_crc'] == entries[0]['first_decoded_crc']
        report.update(id=key, name=item['name'], candidate_id=item['candidate_id'],
                      source_sha256=item['source_sha256'], user_selected=True,
                      source_cache=review['alpha_cache'], source_review=item['review_file'],
                      endpoint_settle_ms=240, full_character_fit=review['fixed_whole_character_scale']*scale,
                      whole_character_scale_adjustment=scale,
                      known_issues=item.get('known_issues', []), joints_or_limbs_edited=False)
        entries.append(report)
        added.append(key)
        ids.add(key)
        print(json.dumps({'id': key, 'frames': report['frames'], 'bytes': report['bytes']}, ensure_ascii=False), flush=True)
    assert len(entries) <= 32, 'Runtime failure mask supports 32 clips'
    manifest = {**base, 'version': 'lively-v1', 'revision': '2026-10-21-v2',
                'base_manifest': str(args.base_library/'manifest.json'),
                'selection': str(args.selection), 'actions': entries,
                'selected_action_count': len(entries)-4, 'added_action_ids': added,
                'deferred': deferred, 'total_bytes': sum(x['bytes'] for x in entries),
                'hardware_validation': False, 'existing_pack_bytes_preserved': True,
                'preview': str(args.output.parent/'NABO-SD-pixels-preview.mp4')}
    (args.output/'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n')
    print(json.dumps({'added': added, 'deferred': deferred, 'total_bytes': manifest['total_bytes']}, ensure_ascii=False))


if __name__ == '__main__':
    main()
