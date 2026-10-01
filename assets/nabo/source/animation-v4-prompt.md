# NABO wrist animation correction v4

Mode: built-in image generator edit with a local reference; transparent background.

Reference: `../wave-v3-arm.png`.
Saved source: `wave-hand-v4.png`.

## Prompt

Edit this transparent NABO sprite cutout. The source currently contains an open raised hand plus a white sleeve and long forearm. Output ONLY the matching open hand and a very short skin-colored wrist stem suitable for inserting into an existing sleeve cuff. Completely remove every white fabric/sleeve/forearm pixel. Preserve the original hand's five fingers, palm proportions, slightly spread friendly greeting pose, soft 3D chibi shading, peach skin, thumb on viewer's right, and front-facing orientation. The wrist stem must be SHORT, about 10 percent of hand height below the palm, rounded cut at its bottom. Do not extend the arm or add an elbow. One isolated hand centered with modest transparent padding, transparent background, no text, no guides, no objects besides this hand. This is a precise replacement sprite, not a new character illustration.

## Registration

The existing body in `wave-rig-v3.png` already has a complete sleeve. `wave-hand-v4.json` registers only the short wrist/hand. The fixed front cuff layer is cropped from the unchanged body with the recorded front-edge polygon. Draw order is body, rotating hand, fixed cuff lip. The rest angle is -20 degrees; motion adds +/-8 degrees around the wrist. Cropping, panel quantization, and rig composition use the normal asset preparation pipeline.

