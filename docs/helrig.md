# Using a ToneRelay box with Helrig

[Helrig](https://helrig.com) is an editor for Line 6 Helix and HX presets. Its desktop app can reach a Helix or HX
pedal through a ToneRelay box over Wi-Fi, so the pedal stays on the stage or in the studio and the editor runs on
your computer.

Helrig is a separate, closed-source project by Yashar Moradi. ToneRelay does not contain any Helrig code. Helrig
uses ToneRelay through the public relay contract in [raw-relay-protocol.md](raw-relay-protocol.md), the same way
any other client could.

Helrig is not affiliated with, endorsed by or sponsored by Line 6 or Yamaha Guitar Group.

## How it works

In relay-only mode the box does not open a session with the pedal. It carries the pedal's USB bulk transfers to
one network client and back, unchanged, one WebSocket message per transfer. Helrig speaks the pedal's protocol
itself. The box needs no knowledge of it.

```
Helrig desktop app  ──  ws://BOX/raw  ──  ToneRelay box (relay only)  ──  USB  ──  Helix / HX
```

The app finds a box from its address, reads `GET /api/relay` (this does not open the pedal), and lists the box as
a device when relay-only mode is on and a pedal is ready. It then connects through `/raw` like a USB connection.

## The Helrig mode

A box can take on the names a Helrig user expects, so nobody has to learn a second product name. It is a saved setting, off by default
(the box keeps the ToneRelay names). Set it once, with the pedal not in use:

```bash
curl -X POST http://<box>/api/relay -H 'Content-Type: application/json' -d '{"mode":"helrig"}'
```

The box restarts. It then announces itself as `helrigbox.local` and "Helrig Box <id>", and its Wi-Fi setup network is
`HelrigBox-<id>` (`<id>` is six hex digits that stay the same for that box). The mode also turns relay-only on. The ports,
the `/raw` protocol and the `_tonerelay._tcp` service are unchanged, so any other client of the relay still finds the box.
To go back, post `{"mode":"tonerelay"}`. Details are in section 3b of [raw-relay-protocol.md](raw-relay-protocol.md).

## What you need

- A ToneRelay box on the ESP32-P4 running a firmware with relay mode, with the pedal plugged into the box.
- Relay only turned on, before the pedal is attached: `POST /api/relay` with `{"on":true}`. In this mode the
  built-in editor and `/ws` do not hold the pedal.
- The box and the computer on the same trusted network. There is no authentication on `/raw` yet.
- Helrig's desktop app. Helrig is free to use. Connecting to a pedal needs Helrig Connect (a one-time purchase) or
  Helrig Pro. That is Helrig's licensing, not ToneRelay's.

## Tested

Bench-tested with an HX Stomp and a Helix LT through an ESP32-P4-NANO over Wi-Fi: listing the box, connecting,
opening the preset the pedal is playing and live editing. The box recovered on its own after the pedal was
unplugged, after the box lost power and after the computer lost Wi-Fi.

Not tested: saving presets to the pedal through a box, long sessions, other boards, other pedals.

## Status

Support for ToneRelay boxes is being built in Helrig. It is not in a Helrig release yet and there is no date.

## Questions

For Helrig, see [helrig.com](https://helrig.com). For the relay itself, open an issue here.
