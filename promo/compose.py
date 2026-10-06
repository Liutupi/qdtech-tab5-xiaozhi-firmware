"""Tab5 x Nabo promo v2 compositor.
Device screen = real firmware UI frames rendered on the host (ui.rgb, 1280x720 RGB24 @30 fps).
Usage: python3 compose.py ui.rgb film.txt out.mp4 [start_frame end_frame]"""
import math, os, sys, subprocess
import numpy as np
import cv2
from PIL import Image, ImageDraw, ImageFont, ImageFilter
from multiprocessing import Pool

HERE = os.path.dirname(os.path.abspath(__file__))
W, H, FPS = 1920, 1080, 30
UIW, UIH = 1280, 720
BEAT = 0.5
TOTAL = 76.5
NF = int(TOTAL * FPS)
FONTS = '/home/user/fonts_src/noto-cjk/Sans/OTF/SimplifiedChinese'
INTER = '/usr/share/fonts/opentype/inter'

def font(kind, size):
    path = {'black': f'{FONTS}/NotoSansCJKsc-Black.otf', 'bold': f'{FONTS}/NotoSansCJKsc-Bold.otf',
            'medium': f'{FONTS}/NotoSansCJKsc-Medium.otf', 'regular': f'{FONTS}/NotoSansCJKsc-Regular.otf',
            'light': f'{FONTS}/NotoSansCJKsc-Light.otf',
            'inter': f'{INTER}/Inter-SemiBold.otf', 'interb': f'{INTER}/InterDisplay-Black.otf',
            'interm': f'{INTER}/Inter-Medium.otf'}[kind]
    return ImageFont.truetype(path, size)

def hexc(s): s = s.lstrip('#'); return np.array([int(s[i:i + 2], 16) for i in (0, 2, 4)], np.float32)
CYAN, AMBER, PINK, VIOLET, MINT, PERI, WHITE = (hexc(c) for c in
    ('62c8e9', 'f7c84d', 'f59ab5', 'a99bff', '89c8a7', '86a8e8', 'f5f9fd'))
MUTED = hexc('9fb3c8')

def clamp(x, a=0.0, b=1.0): return max(a, min(b, x))
def ease_io(x): x = clamp(x); return x * x * (3 - 2 * x)
def ease_out(x): x = clamp(x); return 1 - (1 - x) ** 3
def ease_expo(x): x = clamp(x); return 1 - 2 ** (-10 * x) if x < 1 else 1.0
def lerp(a, b, u): return a + (b - a) * u

# ------------------------------------------------------------------ device texture
FR, GL, GR, GT, GB = 22, 58, 50, 44, 44            # white frame, glass margins (camera on the left)
SX, SY = FR + GL, FR + GT
TW, TH = SX + UIW + GR + FR, SY + UIH + GB + FR
def build_body():
    ss = 2
    w, h = TW * ss, TH * ss
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    z = lambda *v: [int(round(x * ss)) for x in v]
    d.rounded_rectangle(z(0, 0, TW, TH), 58 * ss, fill=(214, 218, 226, 255))
    d.rounded_rectangle(z(2, 2, TW - 2, TH - 2), 56 * ss, fill=(240, 242, 246, 255))
    d.rounded_rectangle(z(9, 9, TW - 9, TH - 9), 50 * ss, fill=(249, 250, 252, 255))
    d.rounded_rectangle(z(FR, FR, TW - FR, TH - FR), 38 * ss, fill=(6, 7, 10, 255))
    d.rounded_rectangle(z(FR + 2, FR + 2, TW - FR - 2, TH - FR - 2), 36 * ss, fill=(11, 13, 18, 255))
    d.rectangle(z(SX - 2, SY - 2, SX + UIW + 2, SY + UIH + 2), fill=(0, 0, 0, 255))
    cy = TH / 2; cx = FR + GL / 2
    d.ellipse(z(cx - 11, cy - 11, cx + 11, cy + 11), fill=(22, 26, 38, 255))
    d.ellipse(z(cx - 6.5, cy - 6.5, cx + 6.5, cy + 6.5), fill=(8, 10, 22, 255))
    d.ellipse(z(cx - 3, cy - 4, cx + 0.5, cy - 0.5), fill=(70, 110, 200, 255))
    for (x, y) in ((FR / 2 + 1, FR / 2 + 1), (TW - FR / 2 - 1, FR / 2 + 1), (FR / 2 + 1, TH - FR / 2 - 1), (TW - FR / 2 - 1, TH - FR / 2 - 1)):
        d.ellipse(z(x - 5.5, y - 5.5, x + 5.5, y + 5.5), fill=(200, 164, 80, 255))
        d.ellipse(z(x - 2.5, y - 2.5, x + 2.5, y + 2.5), fill=(130, 98, 40, 255))
    img = img.resize((TW, TH), Image.LANCZOS)
    a = np.asarray(img).astype(np.float32)
    # soft top-light on the white frame
    yy = np.linspace(1.04, 0.93, TH)[:, None]
    white = a[..., 0] > 150
    for c in range(3):
        a[..., c] = np.where(white, np.clip(a[..., c] * yy, 0, 255), a[..., c])
    return a
BODY = build_body()                      # float32 RGBA (TH, TW, 4)
BODY_A = BODY[..., 3:4] / 255.0
# glass reflection: a faint diagonal band plus a top sheen
gy, gx = np.mgrid[0:TH, 0:TW].astype(np.float32)
GLASS = np.clip(0.05 * (1 - gy / TH) + 0.0, 0, 1)

# ------------------------------------------------------------------ UI frames
class UI:
    def __init__(self, path):
        self.mm = np.memmap(path, dtype=np.uint8, mode='r')
        self.n = self.mm.size // (UIW * UIH * 3)
    def frame(self, t):
        i = int(clamp(round(t * FPS), 0, self.n - 1))
        return np.asarray(self.mm[i * UIW * UIH * 3:(i + 1) * UIW * UIH * 3]).reshape(UIH, UIW, 3)

def load_taps(path):
    taps = []
    for line in open(path, encoding='utf-8'):
        p = line.split()
        if len(p) >= 4 and p[1] == 'touch':
            taps.append((int(p[0]) / 1000.0, int(p[2]), int(p[3])))
    return taps

# ------------------------------------------------------------------ camera / shots
# pose: focus (u, v) in UI pixels lands at screen (px, py); s = scale; yaw/pitch/roll degrees
def P(u, v, px, py, s, yaw=0, pitch=0, roll=0): return np.array([u, v, px, py, s, yaw, pitch, roll], np.float64)
SHOTS = [  # (t0, t1, pose_from, pose_to, ease)
    (0.0, 3.6, P(-40, 360, 760, 560, 2.5, -40, 10, -5), P(300, 380, 1080, 600, 1.15, -18, 6, -2), 'out'),
    (3.6, 9.6, P(290, 400, 1210, 560, 1.45, -12, 4, 0), P(290, 400, 1190, 560, 1.62, -4, 3, 0), 'io'),
    (9.6, 13.4, P(640, 360, 1250, 560, 0.66, -20, 6, 1), P(640, 360, 1235, 560, 0.69, -9, 4, 0), 'io'),
    (13.4, 15.6, P(665, 58, 960, 520, 2.25, -6, 4, 0), P(665, 58, 960, 520, 2.45, 4, 2, 0), 'io'),
    (15.6, 18.0, P(905, 184, 960, 560, 1.95, 8, 3, 0), P(905, 184, 960, 560, 2.1, 2, 2, 0), 'io'),
    (18.0, 21.7, P(640, 360, 1265, 560, 0.66, -14, 5, 0), P(640, 360, 1250, 560, 0.69, -6, 3, 0), 'io'),
    (21.7, 28.0, P(560, 390, 1000, 540, 1.30, -8, 3, 0), P(600, 390, 990, 540, 1.46, 0, 2, 0), 'io'),
    (28.0, 30.0, P(640, 360, 1290, 570, 0.66, 26, 4, -1), P(640, 360, 1275, 570, 0.68, 12, 3, 0), 'expo'),
    (30.0, 37.0, P(680, 545, 980, 470, 1.75, -7, 3, 0), P(680, 520, 980, 470, 1.85, -2, 2, 0), 'io'),
    (37.0, 41.0, P(830, 345, 960, 440, 1.45, 5, 2, 0), P(860, 345, 960, 440, 1.55, -2, 2, 0), 'io'),
    (41.0, 44.0, P(640, 360, 960, 560, 0.86, 0, 3, 0), P(640, 360, 960, 560, 0.80, -8, 4, 0), 'io'),
    (44.0, 48.0, P(840, 378, 960, 430, 1.55, -5, 3, 0), P(840, 400, 960, 430, 1.7, 4, 2, 0), 'io'),
    (48.0, 52.0, P(640, 360, 650, 560, 0.66, 18, 5, 0), P(640, 360, 670, 560, 0.69, 9, 3, 0), 'io'),
    (52.0, 58.0, P(640, 360, 1260, 560, 0.67, -10, 4, 0), P(640, 360, 1240, 560, 0.67, -22, 6, 1), 'io'),
    (58.0, 60.3, P(600, 380, 960, 470, 1.30, -4, 3, 0), P(600, 380, 960, 470, 1.40, 2, 2, 0), 'io'),
    (60.3, 62.0, P(1000, 330, 1000, 400, 1.5, 6, 2, 0), P(1000, 330, 1000, 400, 1.62, 0, 2, 0), 'expo'),
    (62.0, 64.0, P(640, 360, 650, 560, 0.66, 22, 5, 0), P(640, 360, 670, 560, 0.68, 12, 3, 0), 'expo'),
    (64.0, 66.0, P(640, 360, 1270, 560, 0.66, -22, 5, 0), P(640, 360, 1255, 560, 0.68, -12, 3, 0), 'expo'),
    (66.0, 70.0, P(640, 360, 520, 600, 0.44, -24, 10, -2), P(640, 360, 480, 600, 0.40, -34, 12, -3), 'io'),
    (70.0, 76.5, P(640, 360, 960, 690, 0.58, -6, 6, 0), P(640, 360, 960, 690, 0.62, 0, 3, 0), 'out'),
]
def shot_at(t):
    for i, s in enumerate(SHOTS):
        if s[0] <= t < s[1]: return i, s
    return len(SHOTS) - 1, SHOTS[-1]
def pose_at(t):
    i, (t0, t1, a, b, e) = shot_at(t)
    u = (t - t0) / (t1 - t0)
    k = {'io': ease_io, 'out': ease_out, 'expo': lambda x: ease_out(x) * 0.6 + ease_expo(x) * 0.4}[e](u)
    p = a + (b - a) * k
    # gentle hand-held float
    p[5] += 0.8 * math.sin(t * 0.9); p[6] += 0.5 * math.sin(t * 0.7 + 1)
    p[3] += 4 * math.sin(t * 1.1)
    return p, i, t - t0

FOCAL = 2400.0
def project(p):
    u, v, px, py, s, yaw, pitch, roll = p
    ya, pa, ra = math.radians(yaw), math.radians(pitch), math.radians(roll)
    pts = []
    fu, fv = u + SX, v + SY
    for (x, y) in ((0, 0), (TW, 0), (TW, TH), (0, TH)):
        X, Y, Z = (x - fu) * s, (y - fv) * s, 0.0
        X, Z = X * math.cos(ya) + Z * math.sin(ya), -X * math.sin(ya) + Z * math.cos(ya)
        Y, Z = Y * math.cos(pa) - Z * math.sin(pa), Y * math.sin(pa) + Z * math.cos(pa)
        X, Y = X * math.cos(ra) - Y * math.sin(ra), X * math.sin(ra) + Y * math.cos(ra)
        k = FOCAL / (FOCAL + Z)
        pts.append((px + X * k, py + Y * k))
    return np.array(pts, np.float32)

# ------------------------------------------------------------------ background
def radial(w, h, cx, cy, r):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    return np.clip(1 - np.sqrt((x - cx) ** 2 + (y - cy) ** 2) / r, 0, 1)
BG_BASE = None
def bg_base():
    global BG_BASE
    if BG_BASE is None:
        y = np.linspace(0, 1, H)[:, None, None]
        top, bot = hexc('070b14'), hexc('0b1624')
        BG_BASE = (top * (1 - y) + bot * y) * np.ones((1, W, 1), np.float32)
        BG_BASE += radial(W, H, W * 0.5, H * 0.45, W * 0.75)[..., None] * hexc('0e2236') * 0.9
    return BG_BASE
GLOW = None
def glow_sprite():
    global GLOW
    if GLOW is None:
        GLOW = radial(512, 512, 256, 256, 256) ** 2.2
    return GLOW
def add_glow(img, cx, cy, r, color, k):
    g = cv2.resize(glow_sprite(), (int(r * 2), int(r * 2)))
    x0, y0 = int(cx - r), int(cy - r)
    xa, ya, xb, yb = max(0, x0), max(0, y0), min(W, x0 + g.shape[1]), min(H, y0 + g.shape[0])
    if xb <= xa or yb <= ya: return
    img[ya:yb, xa:xb] += g[ya - y0:yb - y0, xa - x0:xb - x0, None] * color * k

rng = np.random.default_rng(5)
DUST = rng.random((70, 4))
GRAIN = [rng.normal(0, 1, (H // 2, W // 2)).astype(np.float32) for _ in range(6)]
VIG = None
def vignette():
    global VIG
    if VIG is None:
        y, x = np.mgrid[0:H, 0:W].astype(np.float32)
        d = np.sqrt(((x - W / 2) / (W / 2)) ** 2 + ((y - H / 2) / (H / 2)) ** 2)
        VIG = np.clip(1.08 - 0.42 * d ** 2.2, 0.35, 1)[..., None]
    return VIG

ACCENT = [CYAN, CYAN, AMBER, AMBER, AMBER, CYAN, CYAN, PINK, PINK, VIOLET, VIOLET, PINK, VIOLET, AMBER, MINT, MINT,
          PERI, VIOLET, CYAN, CYAN]

# ------------------------------------------------------------------ typography
_tc = {}
def text_img(s, kind, size, color=(255, 255, 255), track=0.0):
    key = (s, kind, size, tuple(color), track)
    if key in _tc: return _tc[key]
    f = font(kind, size)
    if track:
        widths = [f.getlength(c) + size * track for c in s]
        w = int(sum(widths)) + 8
    else:
        w = int(f.getlength(s)) + 8
    asc, desc = f.getmetrics()
    h = asc + desc + 8
    im = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(im)
    if track:
        x = 0
        for c, cw in zip(s, widths):
            d.text((x, 4), c, font=f, fill=255); x += cw
    else:
        d.text((0, 4), s, font=f, fill=255)
    a = np.asarray(im).astype(np.float32) / 255.0
    _tc[key] = (a, np.array(color, np.float32))
    return _tc[key]

def blit_text(img, item, x, y, alpha=1.0, anchor='l', grad=None, reveal=1.0):
    a, col = item
    h, w = a.shape
    if anchor == 'c': x -= w / 2
    elif anchor == 'r': x -= w
    # mask reveal: text slides up inside its own line box
    if reveal < 1.0:
        off = int((1 - ease_expo(reveal)) * h * 0.9)
        a = np.vstack([np.zeros((off, w), np.float32), a[:h - off]]) if off > 0 else a
    x, y = int(round(x)), int(round(y))
    xa, ya, xb, yb = max(0, x), max(0, y), min(W, x + w), min(H, y + h)
    if xb <= xa or yb <= ya: return
    sub = a[ya - y:yb - y, xa - x:xb - x, None] * alpha
    if grad is not None:
        g = np.linspace(0, 1, w)[None, xa - x:xb - x, None]
        c = grad[0] * (1 - g) + grad[1] * g
    else:
        c = col
    img[ya:yb, xa:xb] = img[ya:yb, xa:xb] * (1 - sub) + c * sub

def kinetic_block(img, t, t0, t1, x, y, kicker, head, sub, accent, align='l', head_size=88, lines=None):
    """Kicker (tracked Latin) + headline (CJK Black, may wrap via list) + sub; masked reveal in/out."""
    if t < t0 or t > t1: return
    u_in = (t - t0); u_out = (t1 - t)
    fade = clamp(u_out / 0.35)
    yy = y
    if kicker:
        k = text_img(kicker, 'inter', 24, track=0.34)
        kw = k[0].shape[1]
        bar = clamp(u_in / 0.45)
        lx = x if align == 'l' else (x - kw - 56 if align == 'r' else x - kw / 2 - 28)
        if align != 'c':
            x0 = int(lx); x1 = int(lx + 40 * ease_expo(bar))
            img[int(yy + 15):int(yy + 18), x0:x1] = img[int(yy + 15):int(yy + 18), x0:x1] * (1 - fade) + accent * fade
            blit_text(img, (k[0], accent), lx + 56, yy, fade, 'l', reveal=clamp(u_in / 0.5))
        else:
            blit_text(img, (k[0], accent), x, yy, fade, 'c', reveal=clamp(u_in / 0.5))
        yy += 52
    heads = head if isinstance(head, list) else [head]
    for i, line in enumerate(heads):
        it = text_img(line, 'black', head_size)
        grad = (WHITE, accent * 0.55 + WHITE * 0.45) if i == len(heads) - 1 and len(heads) > 1 else None
        blit_text(img, it, x, yy, fade, align, grad=grad, reveal=clamp((u_in - 0.08 - 0.1 * i) / 0.55))
        yy += int(head_size * 1.22)
    if sub:
        it = text_img(sub, 'regular', 32, color=tuple(MUTED))
        a = fade * ease_out(clamp((u_in - 0.45) / 0.5))
        blit_text(img, it, x + (0 if align != 'l' else 2), yy + 10 + 14 * (1 - a), a, align)

def scrim(img, t, t0, t1):
    if t < t0 - 0.3 or t > t1 + 0.3: return
    a = clamp((t - t0 + 0.3) / 0.4) * clamp((t1 + 0.3 - t) / 0.4)
    y0 = H - 420
    g = np.linspace(0, 1, 420)[:, None, None] ** 1.4 * 0.88 * a
    img[y0:] = img[y0:] * (1 - g) + hexc('05080f') * g

def subtitle(img, t, t0, t1, s):
    if t < t0 or t > t1: return
    a = clamp((t - t0) / 0.25) * clamp((t1 - t) / 0.25)
    it = text_img(s, 'medium', 38)
    w = it[0].shape[1]
    y0 = 948 if not (70.0 <= t) else 960
    img[y0 - 14:y0 + 64, int(960 - w / 2 - 30):int(960 + w / 2 + 30)] *= (1 - 0.35 * a)
    blit_text(img, it, 960, y0, a, 'c')

# ------------------------------------------------------------------ photos (hardware)
PH = {}
def photo(name, width):
    key = (name, width)
    if key not in PH:
        im = Image.open(os.path.join(HERE, 'photos', f'official_{name}.png')).convert('RGB')
        h = int(im.height * width / im.width)
        im = im.resize((width, h), Image.LANCZOS)
        card = Image.new('RGBA', (width + 36, h + 36), (0, 0, 0, 0))
        dd = ImageDraw.Draw(card); dd.rounded_rectangle((0, 0, width + 35, h + 35), 28, fill=(255, 255, 255, 255))
        m = Image.new('L', im.size, 0); ImageDraw.Draw(m).rounded_rectangle((0, 0, im.width - 1, im.height - 1), 16, fill=255)
        card.paste(im, (18, 18), m)
        PH[key] = np.asarray(card).astype(np.float32)
    return PH[key]
def blit_rgba(img, rgba, x, y, alpha=1.0):
    h, w = rgba.shape[:2]; x, y = int(x), int(y)
    xa, ya, xb, yb = max(0, x), max(0, y), min(W, x + w), min(H, y + h)
    if xb <= xa or yb <= ya: return
    a = rgba[ya - y:yb - y, xa - x:xb - x, 3:4] / 255.0 * alpha
    img[ya:yb, xa:xb] = img[ya:yb, xa:xb] * (1 - a) + rgba[ya - y:yb - y, xa - x:xb - x, :3] * a

def chip(img, t, t0, x, y, big, small, accent):
    if t < t0: return
    u = ease_expo(clamp((t - t0) / 0.5)); a = clamp((t - t0) / 0.3)
    b = text_img(big, 'bold', 40); s = text_img(small, 'regular', 26, color=tuple(MUTED))
    w = max(b[0].shape[1], s[0].shape[1]) + 64
    x = x + (1 - u) * 60
    x0, y0, x1, y1 = int(x), int(y), int(x + w), int(y + 112)
    xa, xb = max(0, x0), min(W, x1)
    if xb > xa:
        img[y0:y1, xa:xb] = img[y0:y1, xa:xb] * (1 - 0.55 * a) + hexc('0f2133') * 0.55 * a
        img[y0:y1, xa:xa + 4] = img[y0:y1, xa:xa + 4] * (1 - a) + accent * a
    blit_text(img, b, x + 32, y + 12, a)
    blit_text(img, s, x + 32, y + 66, a)

# ------------------------------------------------------------------ frame render
def device_layer(ui_rgb, t, p, taps, accent):
    tex = BODY.copy()
    scr = ui_rgb.astype(np.float32)
    # touch ripples (in UI space)
    for (tt, x, y) in taps:
        dt = t - tt
        if 0 <= dt < 0.6:
            r = 18 + dt * 120; a = (1 - dt / 0.6) ** 1.6
            cv2.circle(scr, (x, y), int(r), (255, 255, 255), 3, cv2.LINE_AA) if False else None
            ov = scr.copy()
            cv2.circle(ov, (x, y), int(r), (255, 255, 255), 4, cv2.LINE_AA)
            cv2.circle(ov, (x, y), int(16 + 8 * (1 - dt / 0.6)), (255, 255, 255), -1, cv2.LINE_AA)
            scr = scr * (1 - 0.55 * a) + ov * 0.55 * a
    tex[SY:SY + UIH, SX:SX + UIW, :3] = scr
    # glass sheen + moving reflection band tied to yaw
    band_x = (0.35 + p[5] / 60.0) * TW
    band = np.exp(-((gx - band_x - (gy - TH / 2) * 0.55) / (TW * 0.10)) ** 2) * 0.10
    sheen = (GLASS + band)[..., None]
    inner = np.zeros((TH, TW, 1), np.float32); inner[FR:TH - FR, FR:TW - FR] = 1
    tex[..., :3] = tex[..., :3] * (1 - sheen * inner) + 255 * sheen * inner
    return tex

def warp(tex_rgba, quad, mip=True):
    quad = np.asarray(quad, np.float32)
    src = np.array([[0, 0], [TW, 0], [TW, TH], [0, TH]], np.float32)
    span = np.linalg.norm(quad[1] - quad[0]) / TW
    t = tex_rgba
    if mip and span < 0.9:
        k = max(span * 1.25, 0.25)
        t = cv2.resize(tex_rgba, (int(TW * k), int(TH * k)), interpolation=cv2.INTER_AREA)
        src = src * k
    M = cv2.getPerspectiveTransform(src, quad)
    return cv2.warpPerspective(t, M, (W, H), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT, borderValue=0)

def render(fi, ui, taps):
    t = fi / FPS
    p, si, tl = pose_at(t)
    accent = ACCENT[si]
    img = bg_base().copy()
    # light: ambient accent glows + a slow cyan back light behind the device
    add_glow(img, 960 + 300 * math.sin(t * 0.21), 300, 900, accent, 0.10)
    add_glow(img, 1500, 950, 700, VIOLET, 0.05)
    quad = project(p)
    c = quad.mean(0)
    add_glow(img, c[0], c[1], 520 * p[4] + 300, accent, 0.20 + 0.06 * math.exp(-((t % BEAT) / BEAT) * 5) * (1 if 28 <= t < 30 or 44 <= t < 52 or 58 <= t < 66 else 0))
    # dust
    for i, (x, y, z, s) in enumerate(DUST):
        xx = (x * W + t * (8 + 20 * z)) % W; yy = (y * H - t * (6 + 14 * z)) % H
        r = 1 + 2.5 * z; a = 0.12 + 0.25 * z * (0.5 + 0.5 * math.sin(t * 1.3 + i))
        cv2.circle(img, (int(xx), int(yy)), int(r), tuple(float(v) for v in (accent * 0.4 + 90) * a), -1, cv2.LINE_AA)
    # device (+ thickness slices + shadow)
    tex = device_layer(ui.frame(t), t, p, taps, accent)
    # shadow
    sh_quad = quad + np.array([0, 60 * p[4] + 40], np.float32)
    sh_quad = (sh_quad - c) * np.array([1.02, 0.96]) + c
    mask = warp((BODY_A[..., 0] * 255).astype(np.float32), sh_quad, mip=False)
    mask = cv2.GaussianBlur(cv2.resize(mask, (W // 4, H // 4)), (0, 0), 14)
    mask = cv2.resize(mask, (W, H))[..., None] / 255.0
    img *= (1 - 0.55 * mask)
    # side thickness: slices behind the front face, tinted
    depth = 26 * p[4]
    for k in range(5, 0, -1):
        dz = depth * k / 5
        pp = p.copy()
        q2 = project(pp)
        # push slice backwards: approximate by scaling quad about vanishing toward screen centre
        q2 = (q2 - np.array([W / 2, H / 2])) * (FOCAL / (FOCAL + dz)) + np.array([W / 2, H / 2])
        sl = warp(np.dstack([np.full((TH, TW, 3), 196, np.float32), BODY_A * 255]), q2)
        a = sl[..., 3:4] / 255.0
        img = img * (1 - a) + sl[..., :3] * (0.70 + 0.05 * k) * a
    front = warp(tex, quad)
    a = front[..., 3:4] / 255.0
    img = img * (1 - a) + front[..., :3] * a
    overlays(img, t)
    # bloom
    small = cv2.resize(img, (W // 4, H // 4), interpolation=cv2.INTER_AREA)
    bright = np.clip(small - 170, 0, None)
    bloom = cv2.GaussianBlur(bright, (0, 0), 9)
    img += cv2.resize(bloom, (W, H)) * 0.45
    # beat punch on drops + cut flash
    cut = min(abs(t - s[0]) for s in SHOTS)
    if t > 0.1 and cut < 0.12 and any(abs(t - x) < 0.12 for x in (28.0, 58.0, 74.0)):
        img += 60 * (1 - cut / 0.12)
    img *= vignette()
    g = cv2.resize(GRAIN[fi % len(GRAIN)], (W, H), interpolation=cv2.INTER_NEAREST)[..., None]
    img += g * 3.2
    # global fades
    f = clamp(t / 1.0) * clamp((TOTAL - t) / 1.6)
    img *= f
    return np.clip(img, 0, 255).astype(np.uint8)

def overlays(img, t):
    kinetic_block(img, t, 0.8, 3.5, 140, 700, 'M5STACK TAB5  ×  NABO', '一块屏，住着一位伙伴', None, CYAN, head_size=80)
    kinetic_block(img, t, 3.9, 9.4, 140, 380, 'PRESENCE', ['走近，', '它就醒来'], '本地人体感应 · 画面不出设备', CYAN)
    subtitle(img, t, 4.85, 9.6, '“你好！我是纳波，很高兴见到你！”')
    kinetic_block(img, t, 9.8, 13.25, 140, 360, 'HOME', ['每天的', '第一眼'], '翻页时钟 · 每日一句 · 临床干货 · 节日提醒', AMBER)
    kinetic_block(img, t, 18.15, 21.6, 140, 360, 'VOICE', ['说一声', '“你好 Nabo”'], '离线唤醒 · 实时语音 · 表情随声而动', CYAN, head_size=80)
    subtitle(img, t, 18.8, 21.7, '你：“你好 Nabo，今天有什么新歌？”')
    kinetic_block(img, t, 21.85, 22.95, 140, 840, None, '等回复时，看一眼手机', None, CYAN, head_size=52)
    scrim(img, t, 21.85, 22.95)
    subtitle(img, t, 23.05, 27.75, 'Nabo：“早上好！今天的 Muse 电台上线啦，现在听吗？”')
    kinetic_block(img, t, 28.05, 29.95, 140, 360, 'MUSE RADIO', ['Muse', '每日音乐电台'], '主播阿南 · 每天 07:30 准时开播', PINK, head_size=86)
    scrim(img, t, 30.2, 41.0); scrim(img, t, 44.2, 47.9); scrim(img, t, 58.1, 61.95)
    kinetic_block(img, t, 30.3, 36.8, 140, 880, None, '原声播客，字幕逐句同步', None, PINK, head_size=60)
    kinetic_block(img, t, 37.2, 40.9, 140, 880, None, '跟着声音律动 · 今日 10 首歌单', None, VIOLET, head_size=60)
    kinetic_block(img, t, 44.2, 47.9, 140, 840, 'MUSIC', '说出歌名，整首播放', None, PINK, head_size=72)
    kinetic_block(img, t, 48.2, 51.85, 1780, 360, 'LYRICS', ['歌词，', '一句一句跟上'], '网易云完整歌曲 · 逐句歌词', VIOLET, align='r', head_size=80)
    kinetic_block(img, t, 52.2, 57.85, 140, 360, 'RADIO', ['网络电台'], '真实电台直播流 · 一触即播', AMBER)
    kinetic_block(img, t, 58.1, 60.25, 140, 840, 'ICU TOOLS', 'ICU 数值工具', None, MINT, head_size=72)
    kinetic_block(img, t, 60.4, 61.95, 140, 860, None, 'eGFR · 氧合 · 血气 · 静脉泵换算', '结果仅供临床复核', MINT, head_size=52)
    kinetic_block(img, t, 62.05, 63.95, 1780, 400, 'IR REMOTE', ['红外', '学习遥控'], '电视 · 空调 · 投影仪', PERI, align='r', head_size=80)
    kinetic_block(img, t, 64.05, 65.95, 140, 400, 'MUSE INBOX', ['消息', '直达桌面'], 'NAS 与 Muse 推送，第一时间送达', VIOLET, head_size=80)
    if 66.0 <= t < 70.0:
        kinetic_block(img, t, 66.1, 69.9, 900, 150, 'HARDWARE', '小身材，全能硬件', None, CYAN, head_size=72)
        chip(img, t, 66.4, 900, 340, '5 英寸 1280×720', '高清触控屏 · LVGL 原生分辨率', CYAN)
        chip(img, t, 66.65, 900, 472, 'ESP32-P4 + ESP32-C6', '双芯协同 · Wi-Fi 6', CYAN)
        chip(img, t, 66.9, 900, 604, '32 MB PSRAM · 16 MB Flash', '本地模型与流畅动画', CYAN)
        chip(img, t, 67.15, 900, 736, '200 万像素摄像头 · 双麦克风', '看得见，也听得清', CYAN)
        if t >= 67.6:
            u = ease_expo(clamp((t - 67.6) / 0.6)); a = clamp((t - 67.6) / 0.3) * clamp((69.9 - t) / 0.3)
            card = photo('back', 290)
            blit_rgba(img, card, 1520 + (1 - u) * 120, 340, a)
            it = text_img('NP-F550 电池 · 7.4V 2000mAh', 'medium', 22)
            blit_text(img, it, 1520 + 163 + (1 - u) * 120, 340 + card.shape[0] + 16, a, 'c')
    if t >= 70.0:
        u = t - 70.0
        a = clamp(u / 0.6) * clamp((TOTAL - 0.3 - t) / 1.2)
        it = text_img('NABO', 'interb', 150, track=0.18)
        blit_text(img, it, 960, 70, a, 'c', grad=(WHITE, CYAN * 0.6 + WHITE * 0.4), reveal=clamp(u / 0.8))
        it2 = text_img('土皮助手 · 你的桌面 AI 伙伴', 'medium', 40, color=(220, 232, 244))
        blit_text(img, it2, 960, 268, a * ease_out(clamp((u - 0.5) / 0.6)), 'c')
        if u > 4.0:
            it3 = text_img('M5Stack Tab5  ·  ESP32-P4  ·  画面为实机固件界面渲染', 'regular', 24, color=(130, 150, 175))
            blit_text(img, it3, 960, 1012, clamp((u - 4.0) / 0.5) * clamp((TOTAL - t) / 1.0), 'c')

G = {}
def init(ui_path, film_path):
    G['ui'] = UI(ui_path); G['taps'] = load_taps(film_path)
def work(fi):
    return render(fi, G['ui'], G['taps']).tobytes()

if __name__ == '__main__':
    ui_path, film_path, out = sys.argv[1:4]
    a = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    b = int(sys.argv[5]) if len(sys.argv) > 5 else NF
    cmd = ['ffmpeg', '-y', '-v', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{W}x{H}', '-r', str(FPS),
           '-i', '-', '-c:v', 'libx264', '-preset', 'slow', '-crf', '16', '-pix_fmt', 'yuv420p', out]
    if out.endswith('.png'):
        init(ui_path, film_path)
        for fi in range(a, b):
            Image.fromarray(render(fi, G['ui'], G['taps'])).save(out.replace('.png', f'_{fi:04d}.png'))
        sys.exit(0)
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    with Pool(4, initializer=init, initargs=(ui_path, film_path)) as pool:
        for n, fr in enumerate(pool.imap(work, range(a, b), chunksize=4)):
            p.stdin.write(fr)
            if n % 150 == 0: print('frame', a + n, flush=True)
    p.stdin.close(); p.wait()
