# Swept Away — a Sweeper's Clock watchface for PebbleOS

A Pebble watchface inspired by Dutch artist **Maarten Baas**' *Sweepers Clock*
(from his *Real Time* series), in which two street sweepers spend twelve hours
pushing piles of rubbish into the shape of an analog clock's hands.

Here, the hands are made of scattered "garbage" pixels, and two little sweeper
sprites walk the dial and broom those pixels into place. The defining rule of the
piece is preserved literally:

> **A garbage pixel only ever moves while a sweeper's broom is touching it.**

Nothing rotates "for free." The clock's target angle creeps clockwise with real
time, but each grain stays frozen where it lies until a broom reaches it and
pushes it — clockwise, never past the target. So the hands are continuously,
physically *swept* into shape.

## How it works

- **Per-grain simulation.** Every grain has a fixed identity (radius along the
  hand, sideways offset, colour) and its own stored angle. Grains never move on
  their own — only `sweep_grain()` advances them, and only when a *down* broom is
  within bristle reach.
- **Human sweeping.** Each sweeper plants the broom a few degrees behind the
  target, pushes *forward* (clockwise) through a short stroke, lifts, steps back,
  and repeats — drifting slowly along the hand's length. No sideways sweeping.
- **Two workers.** Both normally tend the minute hand. Once per slow cycle,
  sweeper B walks — through the center where the hands meet, so it never jumps —
  over to the hour hand, tends it roughly, and walks back.
- **Resolution-independent.** Geometry is resolved from the display at load, so
  the same code fits round and rectangular screens.

## Supported platforms

| SDK platform | Pebble device              | Display          |
|--------------|----------------------------|------------------|
| `emery`      | Pebble Time 2 / **Obelix** | 200×228 rect     |
| `gabbro`     | **Getafix**                | 260×260 round    |

## Build

Requires the [Pebble tool](https://developer.repebble.com/sdk/) and an installed
SDK (4.x):

```sh
pebble build
pebble install --emulator emery     # or: gabbro
```

The build produces `build/sweptaway.pbw` containing both platform binaries.

## Live emulator viewer (optional)

`live_server.py` streams a running emulator as a ~30 fps MJPEG feed in the
browser — handy for showing the face off remotely (e.g. over Tailscale). It
auto-discovers the QEMU monitor port by machine name and reads the frame size
from each screendump, so it works for any platform.

```sh
EMU_MACHINE=pebble-emery EMU_LABEL="emery (Obelix)" LIVE_PORT=8088 python3 live_server.py
# then open http://<host>:8088/
```

Requires Pillow (`pip install pillow`).

## Credit

Concept after Maarten Baas, *Sweepers Clock* / *Real Time*
(<https://maartenbaas.com>). This is a fan homage built for PebbleOS.
