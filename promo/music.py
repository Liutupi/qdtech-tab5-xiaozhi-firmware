"""Tab5 x Nabo promo soundtrack v2: 120 BPM, D major, sampled instruments (GeneralUser GS)
+ synthesized layers, mixed with pedalboard. Writes music.wav (48 kHz stereo) and
level.txt (UI audio level 0..100 every 10 ms, used by the radio/podcast waveforms).
Usage: python3 music.py <GeneralUser-GS.sf2> <greeting.ogg> <out_dir>"""
import sys, os, subprocess
import numpy as np
import tinysoundfont
from pedalboard import (Pedalboard, Reverb, Compressor, Limiter, HighpassFilter, LowpassFilter,
                        Delay, Chorus, HighShelfFilter, LowShelfFilter, PeakFilter, Gain, Distortion)

SF2, GREET, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
SR = 48000
BPM = 120.0
BEAT = 60.0 / BPM
BAR = 4 * BEAT
BARS = 37
DUR = BARS * BAR + 3.0
N = int(DUR * SR)
rng = np.random.default_rng(11)

def t2s(t): return int(round(t * SR))
def mid(n): return 440.0 * 2 ** ((n - 69) / 12)

# ---------------------------------------------------------------- sampled parts
def render_sf(notes, bank, preset, gain_db=0.0, tail=3.0):
    """notes: list of (time_s, midi, vel, dur_s). Returns stereo float array (N,2)."""
    s = tinysoundfont.Synth(samplerate=SR, gain=gain_db)
    sf = s.sfload(SF2)
    ch = 9 if bank == 128 else 0
    s.program_select(ch, sf, bank, preset)
    ev = []
    for t, n, v, d in notes:
        ev.append((t, 1, n, v)); ev.append((t + d, 0, n, 0))
    ev.sort(key=lambda e: (e[0], e[1]))
    out = np.zeros((N, 2), np.float32)
    pos = 0
    for t, on, n, v in ev:
        i = min(N, t2s(t))
        if i > pos:
            out[pos:i] = np.frombuffer(s.generate(i - pos), np.float32).reshape(-1, 2)
            pos = i
        if on: s.noteon(ch, int(n), int(v))
        else: s.noteoff(ch, int(n))
    if pos < N:
        out[pos:] = np.frombuffer(s.generate(N - pos), np.float32).reshape(-1, 2)
    return out

def fx(x, board):
    return board(x.T.astype(np.float32), SR).T

# ---------------------------------------------------------------- harmony
# vi - IV - I - V in D major : Bm  G  D  A
CH = [  # (bass root midi, chord tones mid register)
    (35, [62, 66, 71]),   # Bm : B1 | D4 F#4 B4
    (31, [62, 67, 71]),   # G  : G1 | D4 G4 B4
    (38, [62, 66, 69]),   # D  : D2 | D4 F#4 A4
    (33, [61, 64, 69]),   # A  : A1 | C#4 E4 A4
]
def chord(bar): return CH[bar % 4]

# Nabo motif (4 bars), (beat offset, midi, beats)
MOTIF = [
    [(0, 78, 1), (1, 76, .5), (1.5, 74, .5), (2, 71, 1), (3, 69, .5), (3.5, 71, .5)],
    [(0, 74, 1.5), (1.5, 76, .5), (2, 71, 2)],
    [(0, 81, 1), (1, 78, .5), (1.5, 76, .5), (2, 74, 1), (3, 76, .5), (3.5, 78, .5)],
    [(0, 76, 1.5), (1.5, 73, .5), (2, 69, 2)],
]

# arrangement flags per bar
def sec(bar):
    if bar < 2: return 'intro0'      # 0-4 s   sleeping
    if bar < 5: return 'intro1'      # 4-10 s  wake + greeting
    if bar < 9: return 'home'        # 10-18 s
    if bar < 13: return 'talk'       # 18-26 s dialogue (sparse groove under voices)
    if bar < 14: return 'build'      # 26-28 s riser into the drop
    if bar < 15: return 'drop1'      # 28-30 s Muse title hit
    if bar < 22: return 'bed'        # 30-44 s podcast plays (light groove)
    if bar < 26: return 'drop1'      # 44-52 s the song on the device
    if bar < 28: return 'radio'      # 52-56 s live radio + DJ
    if bar < 29: return 'build'      # 56-58 s
    if bar < 33: return 'drop2'      # 58-66 s tools
    if bar < 35: return 'pre'        # 66-70 s hardware
    if bar < 37: return 'outro'      # 70-74 s
    return 'end'
DROP = {'drop1', 'drop2'}

# ---------------------------------------------------------------- note lists
piano, pad, strings, bass_sf, celeste, kick_sf, clap, hats, ohat, crash, shaker, snare = ([] for _ in range(12))
for bar in range(BARS):
    s = sec(bar); t0 = bar * BAR; root, tones = chord(bar)
    # piano: motif in intro/home/break/outro; chord stabs elsewhere
    if s in ('intro0', 'intro1', 'home', 'break', 'pre', 'outro', 'talk', 'bed', 'radio'):
        for b, n, d in MOTIF[bar % 4]:
            vel = 58 if s.startswith('intro') else 66
            piano.append((t0 + b * BEAT, n, vel, d * BEAT * 1.6))
        for n in tones:
            piano.append((t0, n - 12, 44, BAR * 0.95))
        piano.append((t0, root + 12, 50, BAR))
    if s == 'build':
        for k in range(8):
            piano.append((t0 + k * BEAT / 2, tones[k % 3] + 12, 44 + 3 * k, BEAT * 0.45))
    if s in DROP:
        for k in (0, 1.5, 3):
            for n in tones: piano.append((t0 + k * BEAT, n, 54, BEAT * 0.9))
    # pads / strings
    if s != 'end':
        for n in tones:
            pad.append((t0, n - 12, 70 if s in DROP else 60, BAR))
        if s in ('home', 'build', 'drop1', 'drop2', 'pre', 'outro', 'break', 'talk', 'bed', 'radio'):
            for n in tones + [tones[0] + 12]:
                strings.append((t0, n, 52 if s in ('home', 'break') else 64, BAR * 1.02))
    # sampled bass doubles synth sub in groove sections
    if s in ('home', 'build', 'drop1', 'drop2', 'pre', 'talk', 'bed', 'radio'):
        for k in range(8 if s in DROP else 4):
            st = t0 + k * BAR / (8 if s in DROP else 4)
            bass_sf.append((st, root + 12, 92 if s in DROP else 80, BAR / (8 if s in DROP else 4) * 0.8))
    # celeste sparkle: motif one octave up in drops (every other bar) and outro
    if s in DROP or s == 'outro':
        for b, n, d in MOTIF[bar % 4]:
            celeste.append((t0 + b * BEAT, n + 12, 70, d * BEAT))
    # drums
    if s in ('home', 'talk', 'bed', 'radio'):
        for b in (0, 2): kick_sf.append((t0 + b * BEAT, 36, 90, .2))
        for k in range(16): shaker.append((t0 + k * BEAT / 4, 70, 60 + (24 if k % 2 else 0), .1))
    if s in ('bed', 'radio'):
        for k in range(8): hats.append((t0 + k * BEAT / 2 + BEAT / 4, 42, 44, .05))
    if s == 'build':
        filt = 2
        for b in range(4): kick_sf.append((t0 + b * BEAT, 36, 96, .2))
        if True:
            for b in (1, 3): clap.append((t0 + b * BEAT, 39, 100, .2))
        for k in range(16): hats.append((t0 + k * BEAT / 4, 42, 50 + filt * 10 + (16 if k % 2 == 0 else 0), .05))
        if bar in (13, 28):  # snare roll accelerating
            for k in range(16):
                snare.append((t0 + k * BEAT / 4, 38, 50 + k * 4, .1))
    if s in DROP or s == 'pre':
        for b in range(4): kick_sf.append((t0 + b * BEAT, 36, 112, .2))
        if s != 'pre':
            for b in (1, 3): clap.append((t0 + b * BEAT, 39, 112, .2)); snare.append((t0 + b * BEAT, 40, 70, .2))
            for b in range(4): ohat.append((t0 + b * BEAT + BEAT / 2, 46, 86, .2))
        for k in range(16):
            if s == 'drop2' or k % 2 == 1:
                hats.append((t0 + k * BEAT / 4, 42, 62 + (22 if k % 4 == 2 else 0), .05))
        if bar in (25, 32):
            for k in range(8): snare.append((t0 + 2 * BEAT + k * BEAT / 4, 38, 70 + k * 6, .1))
    if False:
        for k in range(16): snare.append((t0 + k * BEAT / 4, 38, 46 + k * 5, .1))
    if bar in (5, 14, 22, 29, 33, 35):
        crash.append((t0, 49, 104, 2.0))
    if s == 'outro' and bar == 35:
        kick_sf.append((t0, 36, 110, .3))
# final hit
END = BARS * BAR
piano += [(END, 38, 80, 4), (END, 50, 70, 4), (END, 62, 66, 4), (END, 66, 64, 4), (END, 69, 64, 4), (END, 74, 70, 4)]
crash.append((END, 57, 110, 3.0)); kick_sf.append((END, 36, 120, .4))
pad += [(END, 50, 70, 3), (END, 57, 66, 3), (END, 62, 66, 3)]
strings += [(END, 62, 70, 3), (END, 66, 66, 3), (END, 69, 66, 3), (END, 74, 66, 3)]

print('rendering sampled parts...')
P = render_sf(piano, 0, 0, gain_db=2)
PAD = render_sf(pad, 0, 89, gain_db=-2)
STR = render_sf(strings, 0, 49, gain_db=-4)
BSF = render_sf(bass_sf, 0, 38, gain_db=-4)
CEL = render_sf(celeste, 0, 8, gain_db=-6)
DR_K = render_sf(kick_sf, 128, 26, gain_db=0)
DR_C = render_sf(clap, 128, 26, gain_db=0)
DR_S = render_sf(snare, 128, 16, gain_db=-4)
DR_H = render_sf(hats, 128, 26, gain_db=-4)
DR_O = render_sf(ohat, 128, 26, gain_db=-6)
DR_X = render_sf(crash, 128, 16, gain_db=-6)
DR_SH = render_sf(shaker, 128, 0, gain_db=-4)
REV = render_sf([(14 * BAR - 2.0, 60, 90, 2.0), (29 * BAR - 2.0, 60, 90, 2.0), (22 * BAR - 2.0, 60, 70, 2.0)], 0, 119, gain_db=-6)

# ---------------------------------------------------------------- synth layers
tt = np.arange(N) / SR
# sidechain envelope: duck after every kick in groove sections
duck = np.ones(N, np.float32)
kick_times = sorted(set(round(k[0], 4) for k in kick_sf))
for kt in kick_times:
    i0 = t2s(kt); L = t2s(0.32)
    seg = np.linspace(0, 1, L)
    env = 1 - 0.78 * (1 - seg) ** 2.2
    j = min(N, i0 + L)
    duck[i0:j] = np.minimum(duck[i0:j], env[: j - i0])

def saw(freq, n, phase0=0.0):
    ph = (phase0 + np.cumsum(np.full(n, freq / SR))) % 1.0
    return 2 * ph - 1

# synth kick (sub punch) layered under the sampled kick
SKICK = np.zeros(N, np.float32)
for kt in kick_times:
    i0 = t2s(kt); L = t2s(0.45); x = np.arange(L) / SR
    f = 46 + 110 * np.exp(-x * 38)
    ph = 2 * np.pi * np.cumsum(f) / SR
    k = np.sin(ph) * np.exp(-x * 6.5) + 0.25 * np.sin(ph * 2) * np.exp(-x * 30)
    j = min(N, i0 + L); SKICK[i0:j] += (np.tanh(1.6 * k) * 0.9)[: j - i0]

# sub bass: sine following roots, 8th-note offbeat pulses in drops, long notes elsewhere
SUB = np.zeros(N, np.float32)
for bar in range(BARS):
    s = sec(bar)
    if s not in ('home', 'build', 'drop1', 'drop2', 'pre', 'talk', 'bed', 'radio'): continue
    root = chord(bar)[0]; f = mid(root + 12) / 2 if root + 12 > 45 else mid(root + 12)
    f = mid(root)
    t0 = bar * BAR
    i0, i1 = t2s(t0), t2s(t0 + BAR)
    x = np.arange(i1 - i0) / SR
    tone = np.sin(2 * np.pi * f * x) + 0.18 * np.sin(4 * np.pi * f * x)
    SUB[i0:i1] += tone * (0.55 if s in DROP else 0.42)

# supersaw chords (drops) and pluck arp (build/drops)
SUPER = np.zeros((N, 2), np.float32)
ARP = np.zeros((N, 2), np.float32)
dets = [-0.19, -0.11, -0.04, 0.0, 0.04, 0.11, 0.19]
for bar in range(BARS):
    s = sec(bar); root, tones = chord(bar); t0 = bar * BAR
    i0, i1 = t2s(t0), t2s(t0 + BAR)
    n = i1 - i0
    if s in DROP or s == 'pre':
        for note in tones + [tones[0] - 12]:
            f = mid(note)
            for k, d in enumerate(dets):
                w = saw(f * 2 ** (d / 12), n, rng.random())
                pan = (k / (len(dets) - 1)) * 2 - 1
                SUPER[i0:i1, 0] += w * (0.5 - 0.35 * pan) * 0.05
                SUPER[i0:i1, 1] += w * (0.5 + 0.35 * pan) * 0.05
    if s in ('build', 'bed') or s in DROP:
        pattern = [0, 1, 2, 1, 0, 2, 1, 2]
        for k in range(16):
            note = tones[pattern[k % 8]] + (12 if (k // 4) % 2 else 0) + 12
            st = t2s(t0 + k * BEAT / 4); L = t2s(BEAT / 4 * 1.8); x = np.arange(L) / SR
            f = mid(note)
            w = (saw(f, L) * 0.6 + saw(f * 1.004, L) * 0.4) * np.exp(-x * 14)
            j = min(N, st + L)
            g = 0.10 if s in DROP else (0.04 if s == 'bed' else 0.08)
            ARP[st:j, 0] += (w * g)[: j - st]; ARP[st:j, 1] += (w * g)[: j - st]

# risers & impacts
FXL = np.zeros((N, 2), np.float32)
def riser(t_start, t_end, g=0.35):
    """Band-limited noise swell whose low-pass opens over the build (no white-noise wall)."""
    i0, i1 = t2s(t_start), t2s(t_end); L = i1 - i0
    noise = rng.standard_normal((L, 2)).astype(np.float32)
    out = np.zeros_like(noise)
    steps = 24
    for k in range(steps):
        a, b = k * L // steps, (k + 1) * L // steps
        cut = 300 * (9000 / 300) ** (k / (steps - 1))
        seg = Pedalboard([HighpassFilter(150), LowpassFilter(cut), LowpassFilter(cut)])(noise[a:b].T.copy(), SR).T
        out[a:b] = seg
    x = np.linspace(0, 1, L)
    sweep = np.sin(2 * np.pi * np.cumsum(220 + 660 * x ** 2) / SR)[:, None] * 0.25
    FXL[i0:i1] += (out * 0.6 + sweep) * (x ** 3)[:, None] * g
def impact(t, g=0.9):
    i0 = t2s(t); L = t2s(2.5); x = np.arange(L) / SR
    boom = np.sin(2 * np.pi * np.cumsum(30 + 70 * np.exp(-x * 5)) / SR) * np.exp(-x * 1.8)
    j = min(N, i0 + L); FXL[i0:j] += (boom * g)[: j - i0, None]
riser(26 * 1.0 * BAR / BAR * 0 + 13 * BAR, 14 * BAR - BEAT * 0.5, 0.16); riser(28 * BAR, 29 * BAR - BEAT * 0.5, 0.14); riser(21 * BAR, 22 * BAR - BEAT * 0.5, 0.10)
for b in (14, 22, 29): impact(b * BAR, 0.55)
impact(END, 0.7)
# drop gap: silence the last half beat before drops
GAP = np.ones(N, np.float32)
for b in (14, 29):
    i0, i1 = t2s(b * BAR - BEAT * 0.5), t2s(b * BAR)
    GAP[i0:i1] = 0.0

# ---------------------------------------------------------------- mix
print('mixing...')
def st(x): return np.stack([x, x], 1) if x.ndim == 1 else x
hall = Reverb(room_size=0.82, damping=0.45, wet_level=0.32, dry_level=0.75, width=1.0)
plate = Reverb(room_size=0.55, damping=0.5, wet_level=0.22, dry_level=0.85, width=1.0)

P = fx(P, Pedalboard([HighpassFilter(90), PeakFilter(2800, 2.0, 0.8), Compressor(-18, 2.5, 8, 120), hall]))
PAD = fx(PAD, Pedalboard([HighpassFilter(140), LowpassFilter(6000), Chorus(0.4, 0.3, 7, 0.2, 0.3), hall]))
STR = fx(STR, Pedalboard([HighpassFilter(180), hall]))
CEL = fx(CEL, Pedalboard([HighpassFilter(600), Delay(BEAT * 0.75, 0.35, 0.25), hall]))
SUPER = fx(SUPER, Pedalboard([HighpassFilter(170), LowpassFilter(7200), Chorus(0.8, 0.25, 8, 0.25, 0.4), plate]))
ARP = fx(ARP, Pedalboard([HighpassFilter(250), LowpassFilter(9000), Delay(BEAT * 0.75, 0.38, 0.3), plate]))
BSF = fx(BSF, Pedalboard([LowpassFilter(1800), Compressor(-20, 4, 5, 80)]))
SUBs = fx(st(SUB), Pedalboard([LowpassFilter(160), Distortion(4), LowpassFilter(220)]))
KICK = fx(DR_K * 0.9 + st(SKICK) * 0.75, Pedalboard([HighpassFilter(28), Compressor(-12, 4, 1, 60), LowShelfFilter(70, 2.0)]))
CLAP = fx(DR_C + DR_S * 0.7, Pedalboard([HighpassFilter(180), plate]))
HATS = fx(DR_H + DR_O + DR_SH, Pedalboard([HighpassFilter(5000), HighShelfFilter(10000, 2.0)]))
CR = fx(DR_X, Pedalboard([HighpassFilter(400), hall]))
REV = fx(REV, Pedalboard([HighpassFilter(300), plate]))
FXL = fx(FXL, Pedalboard([HighpassFilter(60), Reverb(room_size=0.6, wet_level=0.18, dry_level=0.9)]))

d2 = duck[:, None]
mix = (P * 1.0 + PAD * 0.6 * d2 + STR * 0.62 * d2 + CEL * 0.62 + SUPER * 0.85 * d2 + ARP * 0.9 * d2 +
       BSF * 0.22 * d2 + SUBs * 0.42 * d2 + KICK * 0.72 + CLAP * 0.62 + HATS * 0.75 + CR * 0.42 + FXL * 0.8 + REV * 0.5)
# intro filter feel: gentle low-pass on the first 4 bars is handled by sparse arrangement
mix *= GAP[:, None] * 1.0 + (1 - GAP[:, None]) * 0.0

# real device greeting (Nabo voice) after the wake at 4.0 s
g = subprocess.run(['ffmpeg', '-v', 'error', '-i', GREET, '-f', 'f32le', '-ac', '1', '-ar', str(SR), '-'],
                   capture_output=True).stdout
voice = np.frombuffer(g, np.float32).copy()
voice = fx(st(voice), Pedalboard([HighpassFilter(120), PeakFilter(3000, 2.5, 1.0), Compressor(-20, 3, 3, 80),
                                  Reverb(room_size=0.25, wet_level=0.08, dry_level=1.0)]))
VT = 4.85
vi = t2s(VT)
import json
place = json.load(open(os.path.join(OUT, 'vo', 'placements.json')))
def load_src(name, a, b):
    g = subprocess.run(['ffmpeg', '-v', 'error', '-ss', str(a), '-to', str(b), '-i', os.path.join(OUT, 'audio', name),
                        '-f', 'f32le', '-ac', '1', '-ar', str(SR), '-'], capture_output=True).stdout
    x = np.frombuffer(g, np.float32).copy()
    f = min(len(x) // 4, int(0.04 * SR)); x[:f] *= np.linspace(0, 1, f); x[-f:] *= np.linspace(1, 0, f)
    return x
podcast_fx = Pedalboard([HighpassFilter(90), Compressor(-22, 2.5, 5, 120)])
radio_fx = Pedalboard([HighpassFilter(150), Compressor(-22, 2.5, 5, 120)])
tracks = [(VT, st(voice), 1.25, 0.5)]
for t, src, a, b, kind in place:
    x = load_src(src, a, b)
    if kind == 'podcast': y = fx(st(x), podcast_fx); g = 1.1; dk = 0.28
    else: y = fx(st(x), radio_fx); g = 1.2; dk = 0.18
    tracks.append((t, y, g, dk))
vmask = np.ones(N, np.float32)
for t, y, g, dk in tracks:
    i0 = t2s(t); a = max(0, i0 - t2s(0.25)); b = min(N, i0 + len(y) + t2s(0.35))
    ramp = np.ones(b - a, np.float32) * dk
    r = min(t2s(0.2), (b - a) // 2)
    ramp[:r] = np.linspace(1, dk, r); ramp[-r:] = np.linspace(dk, 1, r)
    vmask[a:b] = np.minimum(vmask[a:b], ramp)
# the podcast and radio play on the device: music sits underneath, darker and softer
dev = np.zeros(N, np.float32)
for a_, b_ in ((30.0, 44.0), (52.0, 58.0)):
    dev[t2s(a_):t2s(b_)] = 1.0
dev = np.convolve(dev, np.ones(t2s(0.3)) / t2s(0.3), 'same')
dark = fx(mix, Pedalboard([LowpassFilter(1800), Gain(-4)]))
mix = mix * (1 - dev[:, None]) + dark * dev[:, None]
mix *= vmask[:, None]
for t, y, g, dk in tracks:
    i0 = t2s(t); j = min(N, i0 + len(y)); mix[i0:j] += y[:j - i0] * g

# UI taps: soft glass clicks (times passed via env file)
taps = []
tp = os.path.join(OUT, 'taps.txt')
if os.path.exists(tp):
    taps = [float(x) for x in open(tp).read().split()]
for t in taps:
    i0 = t2s(t); L = t2s(0.06); x = np.arange(L) / SR
    c = (np.sin(2 * np.pi * 2400 * x) * 0.5 + rng.standard_normal(L) * 0.25) * np.exp(-x * 90)
    j = min(N, i0 + L); mix[i0:j] += (c * 0.18)[: j - i0, None]

master = Pedalboard([HighpassFilter(24), LowShelfFilter(90, 0.8), HighShelfFilter(8000, 2.5),
                     Compressor(threshold_db=-14, ratio=2.0, attack_ms=20, release_ms=200),
                     Gain(3.0), Limiter(threshold_db=-1.5, release_ms=150)])
mix = fx(mix, master) * 0.94
fade = np.ones(N, np.float32); fl = t2s(1.2); fade[-fl:] = np.linspace(1, 0, fl) ** 2
mix *= fade[:, None]
peak = np.abs(mix).max(); rms = np.sqrt((mix ** 2).mean())
print('peak %.3f rms %.1f dBFS' % (peak, 20 * np.log10(rms + 1e-9)))
import wave
pcm = (np.clip(mix, -1, 1) * 32767).astype('<i2')
with wave.open(os.path.join(OUT, 'music.wav'), 'wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes(pcm.tobytes())
# UI level track from the instrumental (kick + bass + chords), 10 ms hop
mono = np.abs(mix.mean(1))
hop = SR // 100
lv = np.array([mono[i:i + hop].mean() for i in range(0, N - hop, hop)])
lv = np.clip(lv / (np.percentile(lv, 98) + 1e-9) * 100, 0, 100).astype(int)
np.savetxt(os.path.join(OUT, 'level.txt'), lv, fmt='%d')
print('done', DUR)
