#!/usr/bin/env python3
"""Render the Tab5 greeting rig using the firmware's C++ poses."""

import base64
import json
import math
from pathlib import Path
import shutil
import subprocess
import tempfile

from PIL import Image, ImageDraw

PARTS = ("legs", "torso", "head", "hand", "cuff")
DRAW_ORDER = ("legs", "torso", "hand", "cuff", "head")


def motion_samples(root: Path, geometry: dict, action: str = "Wave") -> list:
    from prepare_nabo_assets import rig_initializer
    with tempfile.TemporaryDirectory(prefix="nabo-greeting-") as directory:
        source, program = Path(directory) / "samples.cc", Path(directory) / "samples"
        source.write_text('''#include "nabo_animation.h"
#include <cstdio>
int main() {
    constexpr nabo::WaveGeometry geometry = GEOMETRY;
    nabo::Animation animation;
    animation.Start(nabo::Action::Wave, 0);
    std::printf("[");
    for (unsigned t = 0; t <= nabo::Animation::Duration(nabo::Action::Wave); t += 20) {
        const auto pose = nabo::BuildWavePose(geometry, animation.Motion(t));
        std::printf("%s[%u", t ? "," : "", t);
        for (const auto& part : {pose.legs, pose.torso, pose.head, pose.hand, pose.cuff})
            std::printf(",[%d,%d,%d]", part.x, part.y, part.angle);
        std::printf("]");
    }
    std::printf("]");
}
'''.replace("GEOMETRY", rig_initializer(geometry)).replace('Action::Wave', 'Action::' + action).replace('#include <cstdio>',
                                                                   '#include <cstdio>\n#include <initializer_list>'))
        subprocess.run(["c++", "-std=c++17", "-I", str(root / "main/boards/qdtech/tab5"),
                        str(source), "-o", str(program)], check=True)
        return json.loads(subprocess.check_output([str(program)], text=True))


def render_frame(layers: dict, geometry: dict, sample: list) -> Image.Image:
    canvas = Image.new("RGBA", (440, 590), (17, 40, 66, 255))
    for name in DRAW_ORDER:
        if name not in layers:
            continue
        x, y, angle = sample[1 + PARTS.index(name)]
        part = geometry[name]
        px, py = part["pivot_x"], part["pivot_y"]
        jx, jy = 70 + x / 256 + px, 15 + y / 256 + py
        radians = math.radians(angle / 10)
        cosine, sine = math.cos(radians), math.sin(radians)
        matrix = (cosine, sine, px - cosine * jx - sine * jy,
                  -sine, cosine, py + sine * jx - cosine * jy)
        overlay = layers[name].transform(canvas.size, Image.Transform.AFFINE, matrix,
                                         Image.Resampling.BICUBIC)
        canvas.alpha_composite(overlay)
    return canvas.convert("RGB")


def render_preview(root: Path, layers: dict, geometry: dict):
    output = root / "assets/nabo"
    urls = {}
    for name, layer in layers.items():
        path = output / f"wave-v5-{name}.png"
        layer.save(path)
        urls[name] = "data:image/png;base64," + base64.b64encode(path.read_bytes()).decode()
    (output / "wave-v5-geometry.json").write_text(json.dumps(geometry, indent=2) + "\n")
    samples = motion_samples(root, geometry)
    (output / "wave-v5-motion.json").write_text(json.dumps(samples) + "\n")
    frames = [render_frame(layers, geometry, sample) for sample in samples[:-1]]
    if encoder := shutil.which("ffmpeg"):
        command = [encoder, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo",
                   "-pixel_format", "rgb24", "-video_size", "440x590", "-framerate", "50",
                   "-i", "pipe:0", "-an", "-c:v", "libx264", "-preset", "veryfast", "-crf", "18",
                   "-pix_fmt", "yuv420p", "-movflags", "+faststart",
                   str(output / "wave-v5-preview.mp4")]
        process = subprocess.Popen(command, stdin=subprocess.PIPE)
        try:
            for frame in frames:
                process.stdin.write(frame.tobytes())
        finally:
            process.stdin.close()
        if process.wait():
            raise RuntimeError("NABO video encoder failed")
    palette = frames[0].quantize(colors=256)
    encoded = [frame.quantize(palette=palette, dither=Image.Dither.NONE) for frame in frames]
    encoded[0].save(output / "wave-v5-preview.gif", save_all=True, append_images=encoded[1:],
                    duration=20, loop=0, optimize=False)
    check = Image.new("RGB", (1320, 1240), (17, 40, 66))
    for index, time in enumerate((0, 480, 920, 1320, 1740, 2380)):
        x, y = index % 3 * 440, index // 3 * 620
        check.paste(frames[time // 20], (x, y))
        ImageDraw.Draw(check).text((x + 20, y + 600), f"{time} ms", fill="white")
    check.save(output / "wave-v5-pose-check.png")
    html = '''<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>NABO 协调挥手</title><style>
body{margin:0;min-height:100vh;display:grid;place-items:center;background:#0d1b2b;color:#e4edf5;font-family:system-ui,sans-serif}
main{text-align:center}h1{font-size:22px;font-weight:500;margin:16px 0 0}p{color:#96afc5;font-size:14px}
canvas{display:block;width:min(440px,90vw,calc((100dvh - 180px)*0.74576));height:auto;border-radius:24px}
button{background:#69d0e8;color:#0d1b2b;border:0;border-radius:16px;padding:10px 22px;font-size:15px;margin:10px;cursor:pointer}
input{display:block;width:75%;margin:0 auto;accent-color:#69d0e8}
</style><main><h1>NABO · 协调挥手</h1><p>轻轻侧身，点头回应，再回到站姿</p>
<canvas width="440" height="590" aria-label="NABO 身体头部与挥手配合动画"></canvas>
<button type="button" id="pause">暂停</button><button type="button" id="restart">重播</button>
<input type="range" min="0" max="2400" step="20" value="0" aria-label="动画进度">
</main><script>
const points=POINTS, geometry=GEOMETRY, urls=URLS, parts=['legs','torso','head','hand','cuff'];
const order=['legs','torso','hand','cuff','head'], images={};
const canvas=document.querySelector('canvas'), context=canvas.getContext('2d');
const pause=document.querySelector('#pause'), slider=document.querySelector('input');
const duration=points[points.length-1][0], storageKey='nabo-wave-v5';
let start=null, paused=false, held=0, lastSaved=0;
try{held=Math.min(duration-1,Math.max(0,Number(localStorage.getItem(storageKey))||0));}catch(e){}
function draw(phase){
  const index=Math.min(points.length-2,Math.floor(phase/20));
  const fraction=(phase-points[index][0])/20;
  context.fillStyle='#112842';context.fillRect(0,0,440,590);
  for(const name of order){
    const n=1+parts.indexOf(name), a=points[index][n], b=points[index+1][n], g=geometry[name];
    const pose=a.map((v,i)=>v+(b[i]-v)*fraction);
    context.save();context.translate(70+pose[0]/256+g.pivot_x,15+pose[1]/256+g.pivot_y);
    context.rotate(pose[2]*Math.PI/1800);context.drawImage(images[name],-g.pivot_x,-g.pivot_y);context.restore();
  }
  slider.value=Math.floor(phase/20)*20;
}
function tick(time){
  if(start===null){start=time-held;draw(held);requestAnimationFrame(tick);return;}
  const phase=paused?held:(time-start)%duration;draw(phase);
  if(time-lastSaved>250){try{localStorage.setItem(storageKey,phase);}catch(e){}lastSaved=time;}
  requestAnimationFrame(tick);
}
for(const name of parts){images[name]=new Image();images[name].src=urls[name];}
Promise.all(parts.map(name=>images[name].decode())).then(()=>{draw(held);requestAnimationFrame(tick);});
pause.onclick=()=>{
  if(paused){start=performance.now()-held;paused=false;pause.textContent='暂停';}
  else{held=(performance.now()-start)%duration;paused=true;pause.textContent='继续';}
};
document.querySelector('#restart').onclick=()=>{held=0;start=performance.now();paused=false;pause.textContent='暂停';};
slider.oninput=()=>{held=Math.min(duration-1,Number(slider.value));paused=true;pause.textContent='继续';draw(held);};
</script></html>'''
    html = html.replace("POINTS", json.dumps(samples)).replace("GEOMETRY", json.dumps(geometry))
    (output / "wave-v5-preview.html").write_text(html.replace("URLS", json.dumps(urls)))


if __name__ == "__main__":
    from prepare_nabo_assets import ROOT, wave_rig
    render_preview(ROOT, *wave_rig())
