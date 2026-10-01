#!/usr/bin/env python3
"""Preview reaction layers using the same motion sampler as the native UI."""
import base64
import json
from pathlib import Path
import shutil
import subprocess
from PIL import Image, ImageDraw, ImageFont
from preview_nabo_greeting import motion_samples, render_frame

LABELS = {'listen': '聆听', 'think': '思考', 'happy': '开心', 'music': '音乐',
          'wink': '眨眼', 'encourage': '鼓励', 'curious': '好奇', 'comfort': '安慰'}


def render_reactions(root: Path, reactions: dict, version: int = 6):
    output = root / 'assets/nabo'
    movies, data = {}, {}
    labels = {name: LABELS[name] for name in reactions}
    checks = Image.new('RGB', (1320, 2480), '#112842')
    for row, name in enumerate(labels):
        layers, geometry = reactions[name]
        samples = motion_samples(root, geometry, name.title())
        frames = [render_frame(layers, geometry, sample) for sample in samples[:-1]]
        movies[name] = frames
        urls = {}
        for part, layer in layers.items():
            path = output / f'{name}-v{version}-{part}.png'
            layer.save(path)
            urls[part] = 'data:image/png;base64,' + base64.b64encode(path.read_bytes()).decode()
        data[name] = {'geometry': geometry, 'samples': samples, 'urls': urls}
        (output / f'{name}-v{version}-motion.json').write_text(json.dumps(samples) + '\n')
        palette = frames[0].quantize(colors=256)
        encoded = [frame.quantize(palette=palette, dither=Image.Dither.NONE) for frame in frames]
        encoded[0].save(output / f'{name}-v{version}-preview.gif', save_all=True,
                        append_images=encoded[1:], duration=20, loop=0, optimize=False)
        for column, time in enumerate((0, 750, 1350)):
            checks.paste(frames[time // 20], (column * 440, row * 620))
            ImageDraw.Draw(checks).text((column * 440 + 20, row * 620 + 600),
                                       f'{name} {time} ms', fill='white')
    checks.save(output / f'reactions-v{version}-pose-check.png')
    font_path = Path('/System/Library/Fonts/Supplemental/Arial Unicode.ttf')
    font = ImageFont.truetype(str(font_path), 26) if font_path.exists() else ImageFont.load_default()
    gallery = []
    for index in range(90):
        canvas = Image.new('RGB', (720, 1040), '#112842')
        for n, name in enumerate(labels):
            x, y = n % 2 * 360, n // 2 * 520
            canvas.paste(movies[name][index].resize((360, 483), Image.Resampling.LANCZOS), (x, y + 36))
            ImageDraw.Draw(canvas).text((x + 152, y + 6), labels[name], font=font, fill='#d7eefa')
        gallery.append(canvas)
    palette = gallery[0].quantize(colors=256)
    encoded = [frame.quantize(palette=palette, dither=Image.Dither.NONE) for frame in gallery]
    encoded[0].save(output / f'reactions-v{version}-preview.gif', save_all=True,
                    append_images=encoded[1:], duration=20, loop=0, optimize=False)
    if encoder := shutil.which('ffmpeg'):
        process = subprocess.Popen([encoder, '-hide_banner', '-loglevel', 'error', '-y',
            '-f', 'rawvideo', '-pixel_format', 'rgb24', '-video_size', '720x1040',
            '-framerate', '50', '-i', 'pipe:0', '-an', '-c:v', 'libx264',
            '-preset', 'veryfast', '-crf', '18', '-pix_fmt', 'yuv420p', '-movflags', '+faststart',
            str(output / f'reactions-v{version}-preview.mp4')], stdin=subprocess.PIPE)
        try:
            for frame in gallery:
                process.stdin.write(frame.tobytes())
        finally:
            process.stdin.close()
        if process.wait():
            raise RuntimeError('NABO reaction encoder failed')
    html = '''<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>NABO 表情动作</title>
<style>body{margin:0;background:#0d1b2b;color:#d7eefa;font-family:system-ui;text-align:center}
h1{font-size:23px;font-weight:500}p{font-size:14px;color:#96afc5}
main{max-width:760px;margin:auto}.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
canvas{width:100%;height:auto;display:block;border-radius:20px}h2{font-size:18px;margin:6px}
button{background:#69d0e8;border:0;border-radius:12px;padding:10px 24px;margin:12px;font-size:16px}
input{width:65%;accent-color:#69d0e8}section{min-width:0}</style>
<main><h1>NABO · 表情动作</h1><p>聆听点头 · 思考侧倾 · 开心弹动 · 音乐摇摆</p>
<button id="pause">暂停</button><button id="restart">重播</button>
<input aria-label="动画进度" type="range" min="0" max="1800" step="20" value="0">
<div class="grid">PANELS</div></main><script>
const data=DATA, parts=['legs','torso','head','hand','cuff'], images={};
let start=null, paused=false, held=0;
const button=document.querySelector('#pause'), slider=document.querySelector('input');
function draw(phase){for(const [name,d] of Object.entries(data)){
 const ctx=document.querySelector('#'+name).getContext('2d');ctx.fillStyle='#112842';ctx.fillRect(0,0,440,590);
 const i=Math.min(d.samples.length-2,Math.floor(phase/20)), f=(phase-d.samples[i][0])/20;
 for(const part of ['legs','torso','hand','cuff','head']){if(!images[name][part])continue;const n=1+parts.indexOf(part), a=d.samples[i][n],b=d.samples[i+1][n],g=d.geometry[part];
 const p=a.map((v,j)=>v+(b[j]-v)*f);ctx.save();ctx.translate(70+p[0]/256+g.pivot_x,15+p[1]/256+g.pivot_y);
 ctx.rotate(p[2]*Math.PI/1800);ctx.drawImage(images[name][part],-g.pivot_x,-g.pivot_y);ctx.restore();}}
 slider.value=Math.floor(phase/20)*20;}
function tick(t){if(start===null)start=t-held;draw(paused?held:(t-start)%1800);requestAnimationFrame(tick);}
const ready=[];for(const [name,d] of Object.entries(data)){images[name]={};for(const [part,url] of Object.entries(d.urls)){
 const im=new Image();im.src=url;images[name][part]=im;ready.push(im.decode());}}
Promise.all(ready).then(()=>requestAnimationFrame(tick));
button.onclick=()=>{if(paused){start=performance.now()-held;paused=false;button.textContent='暂停';}
 else{held=(performance.now()-start)%1800;paused=true;button.textContent='继续';}};
document.querySelector('#restart').onclick=()=>{held=0;start=performance.now();paused=false;button.textContent='暂停';};
slider.oninput=()=>{held=Math.min(1799,Number(slider.value));paused=true;button.textContent='继续';draw(held);};
</script></html>'''
    panels = ''.join(f'<section><h2>{label}</h2><canvas id="{name}" width="440" height="590" aria-label="NABO {label}动画"></canvas></section>'
                     for name, label in labels.items())
    if version == 7:
        html = html.replace('聆听点头 · 思考侧倾 · 开心弹动 · 音乐摇摆',
                            '俏皮眨眼 · 点头鼓励 · 好奇探身 · 抱心安慰')
    (output / f'reactions-v{version}-preview.html').write_text(html.replace('PANELS', panels).replace('DATA', json.dumps(data)))


if __name__ == '__main__':
    from prepare_nabo_assets import ROOT, reaction_rigs
    rigs = reaction_rigs()
    render_reactions(ROOT, {name: rigs[name] for name in ("listen", "think", "happy", "music")}, 6)
    render_reactions(ROOT, {name: rigs[name] for name in ("wink", "encourage", "curious", "comfort")}, 7)
