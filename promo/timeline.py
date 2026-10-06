"""Tab5 UI event script v3 (film seconds; 120 BPM, bar = 2 s) + tap times + VO placements."""
import os, sys, json
OUT, D = sys.argv[1], sys.argv[2]
ev, taps, vo = [], [], []
def at(t, c, a=''): ev.append((t, c, a))
def tap(t, x, y, hold=90): at(t, 'touch', f'{x} {y} {hold}'); taps.append(t)
def play(t, src, a, b, kind): vo.append((round(t, 3), src, a, b, kind)); return t + (b - a)

at(-6.0, 'preroll'); at(-6.0, 'episodes', f'{D}/episodes.txt'); at(-6.0, 'messages', f'{D}/messages.txt')
at(-6.0, 'levelfile', f'{OUT}/level.txt'); at(-5.9, 'inbox', '0')
at(-5.8, 'fw', '当前版本 v1.0.9 · 已是最新|检查更新|0|0')
at(-5.8, 'sleep', '1'); at(-5.5, 'dailypage', '3')
# wake (transition clip), then greet with the real device voice
at(3.6, 'welcome')
at(4.75, 'playback', '1'); at(9.55, 'playback', '0')
at(11.0, 'dailypage', '6'); at(15.0, 'dailypage', '5')
# voice turn: listen -> user speaks -> waiting (phone clip) -> Nabo answers
tap(18.0, 904, 590)
at(18.2, 'status', '连接中...'); at(18.6, 'status', '聆听中...')
e = 21.7
at(e - 0.2, 'chat', 'user|你好 Nabo，今天有什么新歌？')
at(e + 0.05, 'status', '待命'); at(e + 0.1, 'waiting', '1')
t_reply = 23.0
at(t_reply - 0.05, 'waiting', '0')
at(t_reply, 'status', '说话中...'); at(t_reply, 'playback', '1')
at(t_reply + 0.05, 'chat', 'assistant|早上好！今天的 Muse 电台上线啦，现在听吗？')
e = t_reply + 4.7
at(e + 0.1, 'playback', '0'); at(e + 0.1, 'status', '待命')
tap(27.55, 1055, 305)
tap(27.85, 331, 227)
# podcast: real narrated audio, transcript follows the audio clock
tap(30.0, 1080, 189)
at(30.12, 'radio', '每日音乐电台 · 10月6日|Playing|')
at(30.15, 'podseek', '220'); play(30.15, 'muse_podcast_2026-10-06.mp3', 0.22, 10.58, 'podcast')
at(40.85, 'podseek', '24350'); play(40.85, 'muse_podcast_2026-10-06.mp3', 24.35, 27.0, 'podcast')
# song with synced lyrics
at(43.9, 'page', 'radio')
at(43.95, 'music', f'把今天调亮一点|Muse|{D}/song.lrc')
at(44.0, 'radio', '把今天调亮一点|Playing|')
# live radio with a DJ
tap(51.9, 238, 245)
at(52.0, 'stopmusic'); at(52.05, 'radio', '中国之声|Playing|MP3 · 64 kbps')
play(52.1, 'radio_live_6s.mp3', 0.0, 5.95, 'radio')
# ICU pump calculator
at(57.9, 'page', 'icu4')
def field(i): return (513, 231 + 67 * i)
def key(ch):
    order = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '0', '删除']
    i = order.index(ch); return (460 + 173 * (i % 3), 217 + 83 * (i // 3))
t = 58.1
for fi, val in ((1, '4'), (3, '3'), (4, '80')):
    tap(t, *field(fi)); t += 0.2
    for ch in val: tap(t, *key(ch)); t += 0.2
    tap(t, 766, 553); t += 0.2
tap(t, 513, 634)
at(61.9, 'page', 'ir'); tap(62.4, 452, 282, 160); tap(63.2, 622, 282, 160)
at(63.9, 'page', 'muse')
at(65.9, 'page', 'close'); at(65.9, 'radio', '中国之声|Stopped|'); at(65.95, 'idle')
at(65.95, 'status', '待命'); at(65.95, 'dailypage', '3')
at(70.3, 'touchnabo')
ev.sort(key=lambda e: e[0])
with open(os.path.join(OUT, 'film.txt'), 'w') as f:
    for t, c, a in ev: f.write(f'{int(round(t * 1000))} {c} {a}'.rstrip() + '\n')
open(os.path.join(OUT, 'taps.txt'), 'w').write('\n'.join(f'{x + 0.09:.3f}' for x in taps))
os.makedirs(os.path.join(OUT, 'vo'), exist_ok=True)
json.dump(vo, open(os.path.join(OUT, 'vo/placements.json'), 'w'), ensure_ascii=False)
print(len(ev), 'events', vo)
