# NABO coordinated greeting v5

## Intended gesture

A gentle greeting from the existing NABO doctor character, seen on the Tab5
home screen. The owner identified two problems in earlier previews: an extra
forearm (fixed in v4), and a static body with only the hand moving (v5).

Use the existing artwork and registered short wrist. Keep the head/face,
glasses, NABO cap, coat, badge and shoes. Avoid independent generated poses
that morph the identity. No new bitmap generation is needed for this update;
source pixels are registered into animation layers by the asset pipeline.

## Timing

| Time | Gesture |
| --- | --- |
| 0–180 ms | Small preparatory lean |
| 180–480 ms | Shift toward the raised hand; start the greeting |
| 480–920 ms | Keep waving; head tilts and nods slightly later |
| 920–1740 ms | Small opposite weight shift with a second head response |
| 1740–2400 ms | Quietly return to the original standing position |

The feet stay planted. Torso and head have separate curves; the head responds
after the torso. The wrist keeps its small wave. Smoothstep interpolation
connects body/head key poses; the hand uses the continuous eased wave.

## Layer ownership and rendering

Draw order: legs, torso, hand, cuff front lip, head. The full sleeve belongs to
the torso. An early mask included part of the raised cuff in the head layer;
that was found in pose inspection and corrected before the final exports.
`source/wave-rig-v5.json` records the reviewed mask. The small original skin
patch below the chin overlaps the head layer so tilting doesn't open a neck gap.

Hip rotation transforms the neck and wrist positions. The cuff lip and hand
share the exact world-space wrist pivot. No second forearm is introduced.
The head rotates around its neck pivot and has up to 3 pixels of downward nod.
No whole-character translation or deformation is used to simulate floating.

The same C++ motion/rig functions produce every preview pose. They do not use
wall-clock timer queues, I/O, or dynamic allocation. The native LVGL path uses
four raw rotating image layers and a stationary leg PNG. It still needs real
Tab5 performance and interruption checks after flashing.
