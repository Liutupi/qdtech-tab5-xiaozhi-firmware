#!/usr/bin/env python3
"""Turn AI Passport's NABO FM stations.json into the Tab5 SD-card catalog.

Output: catalog.bin for /sdcard/tab5/nabo/radio-v1/catalog.bin, in the format
read by main/boards/qdtech/tab5/nabo_radio_catalog.h. The Tab5 decodes MP3 only,
so AAC/HLS stations and stations that only work in a browser (health "cors") are
left out.
"""

import argparse
import hashlib
import json
import struct
import zlib
from collections import Counter
from pathlib import Path

MAGIC = b"NFMC"
VERSION = 1
HEADER = struct.Struct("<4sHHHHHHIIIIII")
RECORD = struct.Struct("<IIHHBBBB")
MAX_STATIONS = 4000
MAX_TYPES = 16
MAX_NAME = 95
MAX_URL = 255
MAX_PY = 31
DEVICE_PATH = "/sdcard/tab5/nabo/radio-v1/catalog.bin"
QT_HOSTS = ("qtfm.cn/live/", "qingting.fm/live/")


def stable_id(url):
    """Same rule as nabo_radio_catalog::StableIdForUrl."""
    for host in QT_HOSTS:
        at = url.find(host)
        if at < 0:
            continue
        rest = url[at + len(host):]
        digits = ""
        while rest[len(digits):len(digits) + 1].isdigit() and len(digits) < 10:
            digits += rest[len(digits)]
        if digits and rest[len(digits):len(digits) + 1] == "/" and 0 < int(digits) < 0x80000000:
            return int(digits)
    return 0x80000000 | (zlib.crc32(url.encode()) & 0x7FFFFFFF)


def playable(station):
    url = station.get("url") or ""
    return (str(station.get("codec", "")).lower() == "mp3" and station.get("health") != "cors"
            and url.startswith(("http://", "https://"))
            and not url.lower().split("?")[0].endswith((".aac", ".m3u8", ".m3u", ".pls"))
            and bool((station.get("name") or "").strip()))


def select(stations):
    """Playable stations in source order (hottest first), one per stream URL."""
    kept, skipped, seen_urls, seen_ids = [], Counter(), set(), {}
    for s in stations:
        if not playable(s):
            skipped["codec" if str(s.get("codec", "")).lower() != "mp3" else
                    "cors" if s.get("health") == "cors" else "url"] += 1
            continue
        if s["url"] in seen_urls:
            skipped["duplicate"] += 1
            continue
        sid = stable_id(s["url"])
        if sid in seen_ids:
            raise ValueError(f"stable id {sid:#x} shared by {seen_ids[sid]!r} and {s['name']!r}")
        seen_urls.add(s["url"])
        seen_ids[sid] = s["name"]
        kept.append(s)
    return kept, skipped


def ordered(counter):
    """Most common first, ties by name, for deterministic tables."""
    return [k for k, _ in sorted(counter.items(), key=lambda kv: (-kv[1], kv[0]))]


def encode_text(text, limit, what, name):
    data = text.encode("utf-8")
    if b"\0" in data or len(data) > limit:
        raise ValueError(f"{what} of {name!r} is longer than {limit} bytes or has NUL")
    return data


def build(stations):
    """Return (catalog bytes, report dict)."""
    kept, skipped = select(stations)
    if not kept or len(kept) > MAX_STATIONS:
        raise ValueError(f"need 1..{MAX_STATIONS} playable stations, got {len(kept)}")
    regions = ordered(Counter((s.get("region") or "网络台").strip() or "网络台" for s in kept))
    types = ordered(Counter(t for s in kept for t in s.get("types") or []))
    if len(regions) > 255 or len(types) > MAX_TYPES:
        raise ValueError(f"{len(regions)} regions / {len(types)} types exceed the format")
    region_id = {r: i for i, r in enumerate(regions)}
    type_bit = {t: i for i, t in enumerate(types)}

    pool = bytearray()
    records = []
    for s in kept:
        name = encode_text(s["name"].strip(), MAX_NAME, "name", s["name"])
        url = encode_text(s["url"], MAX_URL, "url", s["name"])
        py = encode_text((s.get("py") or "").lower(), MAX_PY, "py", s["name"])
        mask = 0
        for t in s.get("types") or []:
            mask |= 1 << type_bit[t]
        region = (s.get("region") or "网络台").strip() or "网络台"
        records.append(RECORD.pack(stable_id(s["url"]), len(pool), min(int(s.get("heat") or 0), 0xFFFF),
                                   mask, region_id[region], len(name), len(url), len(py)))
        pool += name + b"\0" + url + b"\0" + py + b"\0"

    regions_offset = HEADER.size
    types_offset = regions_offset + 4 * len(regions)
    index_offset = types_offset + 4 * len(types)
    names_offset = index_offset + RECORD.size * len(records)
    names = bytearray()
    offsets = []
    for text in regions + types:
        offsets.append(names_offset + len(names))
        names += text.encode("utf-8") + b"\0"
    tables = struct.pack(f"<{len(offsets)}I", *offsets)
    resident = tables + b"".join(records) + bytes(names)
    strings_offset = HEADER.size + len(resident)
    header = HEADER.pack(MAGIC, VERSION, HEADER.size, RECORD.size, len(records), len(regions), len(types),
                         regions_offset, types_offset, index_offset, strings_offset, len(pool),
                         zlib.crc32(resident))
    data = header + resident + bytes(pool)
    report = dict(stations=len(records), skipped=dict(skipped), regions=len(regions), types=types,
                  resident_bytes=strings_offset, bytes=len(data),
                  sha256=hashlib.sha256(data).hexdigest(), device_path=DEVICE_PATH)
    return data, report


def read(data):
    """Parse a catalog back into dicts; raises ValueError when it is malformed."""
    if len(data) < HEADER.size:
        raise ValueError("short header")
    (magic, version, header_size, record_size, count, region_count, type_count, regions_offset,
     types_offset, index_offset, strings_offset, strings_size, crc) = HEADER.unpack_from(data)
    if (magic, version, header_size, record_size) != (MAGIC, VERSION, HEADER.size, RECORD.size):
        raise ValueError("not a version 1 catalog")
    if strings_offset + strings_size != len(data):
        raise ValueError("size mismatch")
    if zlib.crc32(data[HEADER.size:strings_offset]) != crc:
        raise ValueError("index checksum mismatch")

    def text_at(offset):
        return data[offset:data.index(b"\0", offset)].decode("utf-8")

    regions = [text_at(o) for o in struct.unpack_from(f"<{region_count}I", data, regions_offset)]
    types = [text_at(o) for o in struct.unpack_from(f"<{type_count}I", data, types_offset)]
    stations = []
    for i in range(count):
        sid, off, heat, mask, region, name_len, url_len, py_len = RECORD.unpack_from(
            data, index_offset + i * RECORD.size)
        raw = data[strings_offset + off:strings_offset + off + name_len + url_len + py_len + 3]
        name, url, py, tail = raw.split(b"\0")
        if tail or (len(name), len(url), len(py)) != (name_len, url_len, py_len):
            raise ValueError(f"station {i} strings malformed")
        stations.append(dict(id=sid, name=name.decode(), url=url.decode(), py=py.decode(), heat=heat,
                             region=regions[region],
                             types=[t for b, t in enumerate(types) if mask & (1 << b)]))
    return dict(regions=regions, types=types, stations=stations)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stations", type=Path, help="design/nabo-radio/catalog/stations.json")
    parser.add_argument("--output", type=Path, required=True, help="where to write catalog.bin")
    args = parser.parse_args()
    if args.output.exists():
        raise SystemExit(f"{args.output} exists; choose a new path")
    source = json.loads(args.stations.read_text(encoding="utf-8"))
    data, report = build(source["stations"] if isinstance(source, dict) else source)
    read(data)
    args.output.write_bytes(data)
    report["source"] = dict(file=args.stations.name, generated=source.get("generated") if isinstance(source, dict)
                            else None, sha256=hashlib.sha256(args.stations.read_bytes()).hexdigest())
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
