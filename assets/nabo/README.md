# Nabo for the Tab5 native desktop

The four original transparent source sheets in `source/` were supplied by the device
owner. They are not firmware instructions. `poses.png` contains sleep and
wake poses; `wave.png` contains standing and greeting poses; `eyes.png`
contains five facial states; `reference.png` is the character guide. Keep
these originals intact when extending the animation set.

`python3 scripts/prepare_nabo_assets.py` converts selected images into
`main/boards/qdtech/tab5/nabo_assets.c`. It embeds one native-resolution
434 × 618 portrait, two 312 × 125 eye overlays, two 48 × 31 mouth overlays
derived from the owner's expression board, three 300 × 561 greeting poses and
one 480 × 261 sleeping pose. It also embeds a short local Opus greeting generated
from `greeting.ogg`.
The application draws the portrait once, changes only the eye patch for a
blink, changes the mouth patch while speaking, and updates small ambient dots.
Original greeting art is retained; the current greeting uses the v5 coordinated rig; sleep shows rising
`z` labels. The pre-rendered art is displayed at 1:1 size; no full-screen
bitmap scaling is used. The top-right content card rotates through a daily
line, a curated historical event when available, and a festival reminder.
All of these features work without the SD card.

## Current interaction set v7

All nine interaction actions now have continuous motion: wave, happy, wink,
encouragement, thinking, curiosity, listening, comfort and music. V5 wave
and the four v6 reaction curves are retained. V7 reuses the existing reaction
art: wink and encouragement add small wrist rotation with the original cuff
in front; curiosity explores with a body/head lean; comfort uses a slow
upper-body breath and nod. These four retain head, arms and torso together,
with planted source legs. No new independently generated pose artwork is used.
`source/reactions-rig-v7.json` contains all eight registered reaction rigs;
the v6 registration is retained as a historical source.

### When actions appear on the home page

| Event | Action |
| --- | --- |
| Confirmed person entering the camera view | Existing greeting wave |
| Short taps on NABO | Wave → wink → encouragement → curiosity, cycling |
| Double tap within 400 ms | Happy |
| Fresh listening state | Listening |
| Recognized user chat message | Thinking |
| Reply emotion happy/laughing/loving | Happy |
| Reply emotion funny/winking | Wink |
| Reply emotion confident/cool/encouraging | Encouragement |
| Reply emotion surprised/shocked/curious | Curiosity |
| Reply emotion sad/crying/comforting | Comfort |
| Reply emotion thinking/confused | Thinking |
| Awake quiet idle, every 18–25 seconds | Curiosity → thinking → wink, cycling |
| Music starts/resumes and the home page is available | Music, then periodic music idle gestures |

Long press retains the presence test. The speaking mouth animation remains
visible during speech. Reply emotions use a single pending slot: the newest
replaces the previous one, and neutral status updates leave it intact. TTS
and the audio playback queue hold the slot until playback drains. It can
then appear in a quiet listening/idle gap after another current pose finishes.
A detected new utterance restores listening and clears an old pending reply;
a recognized question or tap also clears it. Waiting emotions expire after
15 seconds outside speech; a long TTS response refreshes that window.
Sleeping, camera preview and app pages clear it. The implementation is local
to this board's display; protocol and application state contracts are unchanged.
Emotion actions depend on the reply's emotion tag, with no local sentiment
classification added.

`nabo_reactions.h` owns the emotion mapping, idle order and pending slot.
Existing UI image objects are reused when binding the layers, with no per-frame
bitmap construction. All continuous actions target 20 ms updates. Wave is
2.4 seconds; the other actions are 1.8 seconds. The body is still between
idle gestures. Hardware frame cadence and first PNG decode cost remain untested.

Run `python3 scripts/prepare_nabo_assets.py` to regenerate the embedded C/H
and both reaction preview sets. `python3 scripts/preview_nabo_reactions.py`
regenerates only previews. New previews are `reactions-v7-preview.gif`,
`.mp4`, `.html` and `{wink,encourage,curious,comfort}-v7-preview.gif`.
See `validation-v7.md` for validation and the current firmware size/hash.

## Previous continuous reactions v6

The accepted v5 wave choreography and joint registrations are unchanged.
Four existing v2 reaction illustrations now have continuous 1.8-second
motion: listening, thinking, happiness and music. Each illustration has its
own neck and hip registration in `source/reactions-rig-v6.json`; the wave's
neck mask is not reused. Arms stay in the torso layer. Thinking retains the entire head, hand and
upper body in one layer so the chin contact has no cut boundary. Head/torso secondary motion is small, and feet retain
the source pose. The pale coat hems stay with the torso; navy fabric overlaps
at the hip to cover small bounces.

| Reaction | Motion | Existing trigger |
| --- | --- | --- |
| Listening | Lean toward the listener with a small nod | Fresh listening status |
| Thinking | Slow upper-body/head tilt with hand on chin | User chat message or thinking/confused emotion |
| Happy | Two small upper-body bounces and head response | Double tap or happy/laughing/funny emotion |
| Music | Side-to-side upper-body sway and head bob | Idle reaction while music is playing |

In v6, wink, encouragement, curiosity and comfort used static reaction
illustrations. V7 above adds continuous motion for them.
The music reaction lasts 1.8 seconds and appears during idle intervals. Sleep, camera preview, speaking and page
switching retain their existing pose cancellation rules.

The native UI reuses the same layer objects across states and samples the
wall-clock controller every 20 ms while a moving reaction is active. Eleven
new PNG layers are embedded in the app; LVGL decodes and caches them with
its existing bounded image cache. Motion updates position/rotation, with no
per-frame bitmap construction or SD reads. A first decode or a cache miss
still costs time. Cache pressure and audio concurrency need hardware testing.

`python3 scripts/preview_nabo_reactions.py` renders only the new reactions;
`python3 scripts/prepare_nabo_assets.py` regenerates all embedded assets and
previews. `reactions-v6-preview.gif`, `.mp4` and `.html` show all four motions.
The HTML works from a saved local file and includes pause/replay/seek.
Individual previews use `{listen,think,happy,music}-v6-preview.gif`.
See `validation-v6.md` for the historical v6 build and hardware limits.

## Current wave: coordinated greeting v5

V4 fixed the extra forearm, but only the hand moved. V5 rigs the same artwork
into head, torso, planted legs, hand and foreground cuff. No new independent
full-body poses are generated; the character identity stays consistent.
`source/wave-rig-v5.json` records the source masks, hip/neck pivots and the
small neck overlap. The arm and cuff belong to the torso, and share the same
world-space wrist joint. Their connection never stretches.

The greeting lasts 2.4 seconds: a small anticipatory lean, waving while shifting
weight, a delayed head tilt/nod, then settling back to the starting pose. The
shoes stay planted. Torso tilt is within −2.4° to +1.8°, head movement adds a
separate delayed tilt and up to 3 pixels of nodding, and the wrist adds ±8°.
The head, torso, hand and cuff are embedded RGB565A8 for rotation without
repeated PNG decoding. The stationary leg layer is a PNG. Asset sizes:

| Layer | Size | Bytes |
| --- | --- | --- |
| Head | 266 × 242 | 193,116 |
| Torso | 241 × 233 | 168,459 |
| Hand | 74 × 78 | 17,316 |
| Cuff lip | 58 × 64 | 11,136 |
| Legs | 179 × 181 | 26,292 |

`nabo::Animation::Motion()` and `nabo::BuildWavePose()` drive both the firmware
and the exported preview. Positions use 1/256 pixel precision; LVGL placement
rounds to display pixels. Previews interpolate these positions. Animation and
refresh timers target 20 ms while waving. Full upper-body rotation increases
draw work; hardware cadence, audio concurrency and memory still need testing.

Regenerate everything with `python3 scripts/prepare_nabo_assets.py`. Regenerate
only previews with `python3 scripts/preview_nabo_greeting.py`; the previous
`preview_nabo_wave.py` command forwards to it. Outputs:

- `wave-v5-preview.gif` and `wave-v5-preview.mp4`: 2.4 seconds, 20 ms samples.
- `wave-v5-preview.html`: self-contained interactive preview with pause, replay
  and a timeline slider; remembers its playback position.
- `wave-v5-pose-check.png`: six checkpoints across the whole gesture.
- `wave-v5-motion.json` and `wave-v5-geometry.json`: actual controller samples
  and source registration for inspection.
- `wave-v5-browser.png`: browser validation screenshot.

See `animation-v5-notes.md` for the gesture plan and `validation-v5.md` for
checks and hardware limits. V2–V4 artwork and previews remain available.

## Previous wrist correction v4

V3 mistakenly attached a second forearm to a body that already had a complete
sleeve. V4 replaced that layer with only a short wrist and hand generated using
the built-in image generator; source and prompt are retained as
`source/wave-hand-v4.png` and `source/animation-v4-prompt.md`. It used an unchanged
body, moving hand, and a foreground cuff lip. It fixed length but left the
body static, which v5 addresses. Historical checks are in `validation-v4.md`.

## Previous animation expansion v2

`source/wave-v2.png` and `source/reactions-v2.png` were generated with the
built-in image generator using the owner's originals as identity references.
The prompt set is recorded in `source/animation-v2-prompts.md`. Original art
is retained. The new greeting has eight frames played on a 100 ms timeline;
late UI callbacks skip directly to the current frame. The eight reaction
sprites are delight, wink, encouragement, thinking, curiosity, listening,
comfort and enjoying music.

Short taps cycle greeting, wink, encouragement and curiosity. Two taps
within 400 ms show delight. Long press still opens the existing presence
test. Quiet idle reactions occur every 18–25 seconds. Listening/thinking
and supported emotion messages select corresponding brief poses; speaking
keeps the original mouth patches. Sleep and camera preview cancel a pose.

The preparation script slices equal cells, removes detached alpha flecks,
and uses shared crop bounds and scale within each sequence, with actual shoe
baselines aligned. Newly embedded frame RGB channels match the panel's
RGB565 precision; alpha and full-color source sheets are retained. It emits 300 ×
561 PNG frames into the app, runtime frame previews in `frames-v2/`, a
contact sheet in `preview-v2.png` and a loop in `wave-v2-preview.gif`.
Image cache capacity is bounded at 8 MiB and adjusted against free PSRAM.
No PNG files are read from SD during animation. The v2 build used a 25 fps UI timer (the earlier workspace used 20 fps);
this is not a claim of matching full-character frame rate; wave art advances at 10 fps.
Physical display cadence, audio concurrency and PSRAM pressure still need
device testing after flashing the build.

The camera samples a center-cropped 320 × 240 RGB frame once per second in
the Tab5's landscape orientation. Confirmed motion wakes Nabo; after 90
seconds without activity it changes to the sleeping pose. A local person
detector also checks for someone entering the view, and two detections
trigger a greeting. The detector identifies human presence, not identity.
**测试感应** temporarily shows a camera preview so the owner can check its
framing. Camera frames are not saved or uploaded by this service; no SD card
or face enrollment is needed. An unused `face.db` left on an SD card by an
earlier experimental firmware is never read by this version.

The essential assets reside in the app partition, so Nabo appears without an
SD card. Future optional animation packs can live under `/sdcard/nabo/v1/`.
Such a pack should include a versioned manifest with frame dimensions,
duration, and SHA-256 checksums; load and decode the frames into PSRAM once
after SD mount, and retain the embedded portrait as fallback. Keep frequent
motion restricted to small overlays and do not stream full PNG frames from SD
at display frame rate.

The artwork is supplied for this Tab5 project. Confirm redistribution terms
with the owner before distributing the source sheets or firmware image.

## 2026-10-01 刷机内存修正

已将 NABO v7 刷入 Tab5。主立绘、睡姿及眼部使用 RGB565A8；连续动作替代旧整身帧引用，动作结束会释放 PNG 图层缓存，缓存上限 1 MiB。人物模型增加内存预检并显式加载。最终启动观察 150 秒未重启，人物推理完成多轮。详细固件 SHA-256、验证结果和剩余语音/触摸/帧率范围见 [flash-v7.md](flash-v7.md)。
