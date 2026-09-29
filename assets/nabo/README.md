# Nabo for the Tab5 native desktop

The four transparent source sheets in `source/` were supplied by the device
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
A short greeting switches between the three standing poses; sleep shows rising
`z` labels. The pre-rendered art is displayed at 1:1 size; no full-screen
bitmap scaling is used. The top-right content card rotates through a daily
line, a curated historical event when available, and a festival reminder.
All of these features work without the SD card.

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
