# The hero's animation (v0.9.7)

How Raw Danger animates its characters, and what the PC version adds. Addresses are the US program's.

## The game's animation

- The whole game steps 20 times a second: every third frame the animation (0x324ED0) writes each bone's angles (+0x90),
  0x323B64 makes its local matrix (+0x50..+0x80), and 0x323E40 builds the world matrices, bone by bone in the model's list,
  with 0x323FB0 (world = parent's world x local). The picture is drawn from them the same step.
- The bones (both heroes): 4 the hips, 5 the spine, 6 the chest, 7 the head, 8-11 and 12-15 the legs (hip, knee, ankle, toe),
  16-22 and 23-29 the arms (collar, shoulder, elbow, wrist, hand...), 30-47 hanging parts (the coat's tails, the skirt), in
  threes. y points down.

## What the PC version adds

The call to 0x323FB0 goes through the engine (host call 202, `[Head follows camera]`), which for the player's bones:

| Layer | Where | How much |
|---|---|---|
| Smoothing | Every bone, before its build | A local turn that jumps more than 23 degrees in one step goes half the way each step |
| Breathing | The chest | About 1 degree, every 4.2 s standing still (every 2.6 s when cold), smaller when moving |
| Wet hunch | The spine and the chest | Up to 3 and 3.5 degrees forward as the clothes soak (eased over a few seconds) |
| Shiver | The chest | About 0.7 degree, quick, as the body temperature falls below 24000 (of 30000) |
| Lean into turns | The spine | Up to 8 degrees toward the inside of a turn, by turn rate x speed |
| Head follows camera | The head | docs/CAMERA.md |

The chest and the spine are turned in the world after they are built, about their own joint, so the head and the arms built
after them follow. Only while playing with the follow camera; cutscenes and the first-person view are as the game makes them.
Settings > Controls > Livelier hero (keys.ini `anim_layers`); Settings > Debug > the layers' size (x4, to see them).

## Next

The weight shift standing still, hair and coat sway (the hanging bones 30-47), feet on slopes, the other characters; new clips
(the animation format, retargeting to these bones).
