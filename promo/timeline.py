"""Tab5 UI event script for the v1.0.11 promo (film seconds; 120 BPM, bar = 2 s).
Writes film.txt (host harness), taps.txt (click sounds) and vo/placements.json (real audio)."""
import os, sys, json
OUT, D = sys.argv[1], sys.argv[2]
ev, taps, vo = [], [], []
# The v1.0.11 action segment (28-40 s) is rendered separately (acts.txt); everything
# scripted from 28 s on plays 12 s later in the film.
ACTS = 12.0
def S(t): return t + ACTS if t >= 27.99 else t
def at(t, c, a=''): ev.append((S(t), c, a))
def tap(t, x, y, hold=90): at(t, 'touch', f'{x} {y} {hold}'); taps.append(S(t))
def play(t, src, a, b, kind): vo.append((round(S(t), 3), src, a, b, kind)); return t + (b - a)
def sky(t, time=None, date=None, weather=None):
    if date: at(t, 'setdate', date)
    if time: at(t, 'settime', time)
    if weather is not None: at(t, 'weather', f'{weather} 362 1095')
    at(t + 0.01, 'skyrefresh')

at(-6.0, 'preroll'); at(-6.0, 'episodes', f'{D}/episodes.txt'); at(-6.0, 'messages', f'{D}/messages.txt')
at(-6.0, 'levelfile', f'{OUT}/level.txt'); at(-6.0, 'mdcatalog', f'{D}/md.tsv')
at(-5.95, 'homecatalog', f'{D}/home.tsv'); at(-5.9, 'inbox', '0')
at(-5.8, 'fw', '当前版本 v1.0.11 · 已是最新|检查更新|0|0')
sky(-5.8, time='06:02:00', weather=0)
at(-5.8, 'sleep', '1'); at(-5.5, 'dailypage', '3')
# 0-10 s: dawn; someone walks up -> wake (transition clip) -> greeting (real device voice)
at(3.6, 'welcome'); at(4.75, 'playback', '1'); at(9.55, 'playback', '0')
# 10-18 s: home in daylight
sky(10.0, time='08:15:00')
at(11.0, 'dailypage', '6'); at(15.0, 'dailypage', '5')
# 18-28 s: the sky behind Nabo follows the real world
sky(18.0, time='12:30:00', weather=0)
sky(19.6, weather=3)
sky(21.2, weather=61)
sky(22.8, time='18:22:00', weather=0)
sky(24.4, time='21:30:00', weather=0)
for i, d in enumerate(('2026-10-14', '2026-10-18', '2026-10-22', '2026-10-26', '2026-10-30')):
    sky(25.6 + 0.48 * i, date=d)
sky(28.0, date='2026-10-06', time='08:16:30', weather=0)
# 28-38 s: voice turn (listening: A16 cups an ear; waiting: A05R1 chin on hand; answer)
tap(28.0, 904, 590)
at(28.2, 'status', '连接中...'); at(28.6, 'status', '聆听中...')
at(31.5, 'chat', 'user|你好 Nabo，今天有什么新歌？')
at(31.75, 'status', '待命'); at(31.8, 'waiting', '1')
at(34.75, 'waiting', '0'); at(34.8, 'status', '说话中...'); at(34.8, 'playback', '1')
at(34.85, 'chat', 'assistant|早上好！今天的 NABO 电台上线啦，现在听吗？')
at(37.6, 'playback', '0'); at(37.6, 'status', '待命')
tap(37.55, 1055, 305); tap(37.85, 331, 227)
# 40-54 s: NABO 电台, real podcast audio; transcript follows the audio clock
tap(40.0, 1080, 189)
at(40.12, 'radio', '每日音乐电台 · 10月6日|Playing|')
at(40.15, 'podseek', '220'); play(40.15, 'muse_podcast_2026-10-06.mp3', 0.22, 10.58, 'podcast')
at(50.85, 'podseek', '24350'); play(50.85, 'muse_podcast_2026-10-06.mp3', 24.35, 27.0, 'podcast')
# 54-62 s: a song with synced lyrics
at(53.9, 'page', 'radio'); at(53.95, 'music', f'把今天调亮一点|Muse|{D}/song.lrc'); at(54.0, 'radio', '把今天调亮一点|Playing|')
# 62-68 s: live radio (real recording)
tap(61.9, 238, 245); at(62.0, 'stopmusic'); at(62.05, 'radio', '中国之声|Playing|MP3 · 64 kbps')
play(62.1, 'radio_live_6s.mp3', 0.0, 5.95, 'radio')
# 68-72 s: ICU pump calculator on the real keypad
at(67.9, 'page', 'icu4')
def field(i): return (513, 231 + 67 * i)
def key(ch):
    order = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '0', '删除']
    i = order.index(ch); return (460 + 173 * (i % 3), 217 + 83 * (i // 3))
t = 68.1
for fi, val in ((1, '4'), (3, '3'), (4, '80')):
    tap(t, *field(fi)); t += 0.2
    for ch in val: tap(t, *key(ch)); t += 0.2
    tap(t, 766, 553); t += 0.2
tap(t, 513, 634)
# 72-78 s: 米家中控 (Home Assistant scenes and devices) and its IR remote
at(71.9, 'radio', '中国之声|Stopped|'); at(71.9, 'page', 'home')
tap(72.5, 608, 208, 140)     # 家庭影院 · 开
tap(73.3, 565, 632, 140)     # 客厅主灯 · 关闭
tap(74.0, 755, 440, 140)     # 卧室空调 +
tap(74.5, 755, 440, 140)
tap(75.3, 893, 48, 120)      # 红外遥控
tap(76.4, 452, 282, 160); tap(77.2, 622, 282, 160)
# 78-82 s: games: FC list, then MD
at(77.9, 'page', 'games')
tap(80.0, 893, 259, 120); at(80.1, 'gamemode', 'md')
# 82-84 s: NABO 每日推送
at(81.9, 'page', 'muse')
# 84-86 s: settings / OTA
at(83.9, 'page', 'settings')
# 86 s: back home; 90.3 s: Nabo waves goodbye
at(85.9, 'page', 'close'); at(85.95, 'idle'); at(85.95, 'status', '待命'); at(85.95, 'dailypage', '3')
at(90.3, 'touchnabo')
ev.sort(key=lambda e: e[0])
with open(os.path.join(OUT, 'film.txt'), 'w') as f:
    for t, c, a in ev: f.write(f'{int(round(t * 1000))} {c} {a}'.rstrip() + '\n')
open(os.path.join(OUT, 'taps.txt'), 'w').write('\n'.join(f'{x + 0.09:.3f}' for x in taps))
os.makedirs(os.path.join(OUT, 'vo'), exist_ok=True)
json.dump(vo, open(os.path.join(OUT, 'vo/placements.json'), 'w'), ensure_ascii=False)
print(len(ev), 'events', len(taps), 'taps')
