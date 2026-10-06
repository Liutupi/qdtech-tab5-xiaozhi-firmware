#!/usr/bin/env python3
"""Resume a Muse MP3 upload through short HTTPS requests, using only Python stdlib."""
import argparse
import hashlib
import http.client
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request


def credentials():
    mcp = os.environ.get('MUSE_MCP_URL', '')
    if mcp:
        parsed = urllib.parse.urlsplit(mcp)
        parts = parsed.path.strip('/').split('/')
        if len(parts) != 2 or parts[0] != 'mcp':
            raise ValueError('MUSE_MCP_URL must end with /mcp/<token>')
        return urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, '', '', '')), parts[1]
    base = os.environ.get('MUSE_RELAY_BASE', '').rstrip('/')
    token = os.environ.get('MUSE_TOKEN', '')
    if not base or not token:
        raise ValueError('Set MUSE_MCP_URL, or MUSE_RELAY_BASE and MUSE_TOKEN')
    return base, token


class Uploader:
    def __init__(self, base, token, rate_limit=0):
        self.base, self.token, self.rate_limit = base, token, rate_limit

    def request(self, method, route, value=None, binary=None, digest=None):
        for attempt in range(5):
            headers = {'Authorization': 'Bearer ' + self.token, 'Accept': 'application/json'}
            data = binary if binary is not None else (json.dumps(value).encode() if value is not None else None)
            if binary is not None:
                headers.update({'Content-Type': 'application/octet-stream', 'X-Chunk-SHA256': digest})
            elif data is not None:
                headers['Content-Type'] = 'application/json'
            # Iterator bodies make the optional verification rate limit apply to
            # the socket upload itself, rather than sleeping between requests.
            if binary is not None and self.rate_limit:
                headers['Content-Length'] = str(len(binary))
                data = self.paced(binary)
            req = urllib.request.Request(self.base + route, data=data, method=method, headers=headers)
            try:
                with urllib.request.urlopen(req, timeout=45) as response:
                    result = json.load(response)
                    if not result.get('ok'):
                        raise ValueError(result.get('error', 'upload failed'))
                    return result
            except urllib.error.HTTPError as error:
                if error.code not in (408, 429, 500, 502, 503, 504, 520, 522, 524) or attempt == 4:
                    message = error.read().decode(errors='replace').replace(self.token, '[token]')
                    raise RuntimeError(f'HTTP {error.code}: {message}') from None
                print(f'Retrying {method} after HTTP {error.code}', flush=True)
            except (urllib.error.URLError, TimeoutError, ConnectionError, OSError, http.client.HTTPException) as error:
                if attempt == 4:
                    raise RuntimeError(f'Network upload failed: {type(error).__name__}') from None
                print(f'Retrying {method} after network interruption', flush=True)
            time.sleep(2 ** attempt)

    def paced(self, data):
        started = time.monotonic()
        for offset in range(0, len(data), 8192):
            piece = data[offset:offset + 8192]
            yield piece
            deadline = started + (offset + len(piece)) / self.rate_limit
            time.sleep(max(0, deadline - time.monotonic()))

    def upload(self, file, episode_id=None):
        size = file.stat().st_size
        if size < 3 or size > 32 * 1024 * 1024:
            raise ValueError('MP3 must be between 3 bytes and 32 MiB')
        hash_value = hashlib.sha256()
        with file.open('rb') as source:
            for piece in iter(lambda: source.read(65536), b''):
                hash_value.update(piece)
        route = '/podcast/audio/uploads'
        state = self.request('POST', route, {'filename': file.name, 'size': size,
                             'sha256': hash_value.hexdigest(), 'episode_id': episode_id})
        upload_route = route + '/' + state['upload_id']
        if state['state'] == 'completed':
            print('Already complete; stored receipt verified.', flush=True)
            return state['result']
        with file.open('rb') as source:
            for index in state['missing']:
                source.seek(index * state['chunk_size'])
                piece = source.read(min(state['chunk_size'], size - source.tell()))
                self.request('PUT', f'{upload_route}/chunks/{index}', binary=piece,
                             digest=hashlib.sha256(piece).hexdigest())
                print(f'Chunk {index + 1}/{state["chunk_count"]} saved ({len(piece)} bytes)', flush=True)
        receipt = self.request('POST', upload_route + '/complete', {})
        if receipt['size'] != size or receipt['sha256'] != hash_value.hexdigest():
            raise RuntimeError('Server receipt does not match the source MP3')
        return receipt


def load_transcript(file):
    if file.stat().st_size > 64 * 1024:
        raise ValueError('Transcript file exceeds 64 KiB')
    text = file.read_text(encoding='utf-8-sig').replace('\r\n', '\n').replace('\r', '\n')
    if file.suffix.lower() == '.json':
        value = json.loads(text)
        if isinstance(value, dict) and 'lines' in value:
            import math
            if not isinstance(value['lines'], list):
                raise ValueError('Timeline lines must be an array')
            cues = []
            for line in value['lines']:
                if not isinstance(line, dict):
                    raise ValueError('Timeline line must be an object')
                start, end = line.get('start'), line.get('end')
                if (type(start) not in (int, float) or type(end) not in (int, float)
                        or not math.isfinite(start) or not math.isfinite(end)
                        or not 0 <= start < end <= 14400 or line.get('kind') not in ('speech', 'music')):
                    raise ValueError('Invalid timeline timestamps or speech/music kind')
                cues.append({'start_ms': int(start * 1000 + 0.5), 'end_ms': int(end * 1000 + 0.5),
                             'text': line.get('text'), 'kind': line['kind']})
        else:
            cues = value.get('cues') if isinstance(value, dict) else value
    else:
        cues = []
        stamp = r'(?:(\d{1,2}):)?(\d{2}):(\d{2})[.,](\d{3})'
        timing = re.compile(r'^' + stamp + r'\s+-->\s+' + stamp + r'(?:\s+.*)?$')
        for block in re.split(r'\n\s*\n', text.strip()):
            lines = block.split('\n')
            if lines[0].strip() == 'WEBVTT' or lines[0].startswith(('NOTE', 'STYLE', 'REGION')):
                continue
            index = 0 if '-->' in lines[0] else 1
            match = timing.fullmatch(lines[index].strip()) if len(lines) > index else None
            if not match or len(lines) <= index + 1:
                raise ValueError('Invalid SRT/VTT cue; use ordinary millisecond timestamps')
            def milliseconds(groups):
                hours, minutes, seconds, millis = groups
                if int(minutes) >= 60 or int(seconds) >= 60:
                    raise ValueError('Invalid subtitle timestamp')
                return ((int(hours or 0) * 60 + int(minutes)) * 60 + int(seconds)) * 1000 + int(millis)
            spoken = '\n'.join(lines[index + 1:]).strip()
            if '<' in spoken or '>' in spoken:
                raise ValueError('Use plain spoken text without subtitle markup')
            cues.append({'start_ms': milliseconds(match.groups()[:4]),
                         'end_ms': milliseconds(match.groups()[4:]), 'text': spoken})
    if not isinstance(cues, list) or not 1 <= len(cues) <= 200:
        raise ValueError('Transcript needs 1..200 timed sentences')
    previous_end, total = 0, 0
    for cue in cues:
        if not isinstance(cue, dict):
            raise ValueError('Each transcript cue must be an object')
        start, end, spoken = cue.get('start_ms'), cue.get('end_ms'), cue.get('text')
        if (type(start) is not int or type(end) is not int or not previous_end <= start < end <= 14400000
                or not isinstance(spoken, str) or not spoken.strip() or '\0' in spoken):
            raise ValueError('Invalid transcript cue times or spoken text')
        size = len(spoken.encode())
        total += size
        if size > 1536 or total > 9000:
            raise ValueError('Transcript text exceeds 1536 bytes per cue or 9000 bytes total')
        previous_end = end
    return cues


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('file', type=Path)
    parser.add_argument('--episode-id', type=int, help='Existing episode returned by tab5_podcast_publish')
    parser.add_argument('--rate-limit', type=int, default=0, help='Optional upload bytes/sec for verification')
    parser.add_argument('--result-file', type=Path, help='Private file for the full result, including audio_url')
    parser.add_argument('--speech-optimize', action='store_true',
                        help='Create a 48 kbps, 24 kHz mono playback copy with ffmpeg; preserve the source')
    parser.add_argument('--transcript', type=Path,
                        help='Timed spoken sentences in JSON, SRT, or WebVTT, attached to this audio')
    args = parser.parse_args()
    if args.rate_limit < 0:
        parser.error('--rate-limit cannot be negative')
    if args.transcript and args.episode_id is None:
        parser.error('--transcript requires --episode-id')
    try:
        cues = load_transcript(args.transcript) if args.transcript else None
        base, token = credentials()
        uploader = Uploader(base, token, args.rate_limit)
        if args.speech_optimize:
            ffmpeg = shutil.which('ffmpeg')
            if not ffmpeg:
                raise ValueError('--speech-optimize requires ffmpeg on the generating VM')
            with tempfile.TemporaryDirectory(prefix='muse-speech-') as temporary:
                optimized = Path(temporary) / (args.file.stem + '-tab5.mp3')
                converted = subprocess.run([ffmpeg, '-hide_banner', '-loglevel', 'error', '-nostdin',
                    '-i', str(args.file), '-map', '0:a:0', '-map_metadata', '0', '-vn',
                    '-ac', '1', '-ar', '24000', '-c:a', 'libmp3lame', '-b:a', '48k', str(optimized)],
                    capture_output=True, text=True)
                if converted.returncode:
                    raise RuntimeError('Speech copy conversion failed; source preserved')
                print(f'Speech playback copy: {optimized.stat().st_size} bytes (48 kbps mono)', flush=True)
                result = uploader.upload(optimized, args.episode_id)
        else:
            result = uploader.upload(args.file, args.episode_id)
        if args.result_file:
            fd = os.open(args.result_file, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, 'w') as output:
                json.dump(result, output, ensure_ascii=False, indent=2)
        if cues is not None:
            attached = uploader.request('POST', f'/podcast/transcript?episode_id={args.episode_id}',
                                        {'audio_sha256': result['sha256'], 'cues': cues})
            result['cue_count'] = attached['cue_count']
        # Do not leak the bearer-equivalent audio path into task/cron logs.
        print(json.dumps({key: value for key, value in result.items() if key != 'audio_url'}, ensure_ascii=False), flush=True)
    except (ValueError, RuntimeError, OSError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
