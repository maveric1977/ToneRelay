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

## Setting up a new Helrig Box

A box that is set up as a Helrig Box (see below) starts like this:

1. Plug the box in. It raises its own Wi-Fi network, **`HelrigBox-<id>`** (six hex digits, the same ones that follow "Helrig Box"
   in the app).
2. On your phone, join that network and open `http://192.168.4.1`. The page lists the 2.4 GHz networks the box can see.
3. Pick yours, type the password, and press **Connect**. The page says when the box is on your network and shows its address.
4. Open Helrig on your computer, plug your pedal into the box, and choose the box in the device menu. Helrig finds it by itself
   (`_helrigbox._tcp`); you do not type an address.

45 seconds after it has joined your network the box switches its own network off [measured on the box's serial log: 45.0 s]. Phones and
computers may keep listing the name for a few minutes after it has gone; that is their cached list, not the box. If its Wi-Fi is lost for 40 seconds, for
example after a new router, the network comes back and you repeat steps 2 and 3. While the box is on your network the same page is
at the box's address and offers another network.

**Factory reset.** Hold the board's **BOOT** button for 5 seconds while the box is running. It forgets your Wi-Fi network and
hotspot password, restarts and raises its setup network again. It stays a Helrig Box. (Do not hold BOOT while powering the board up:
that starts the chip's download mode.)

**Handing a box over.** After the application is flashed, `esp32-p4/scripts/provision.sh --mode helrig -p <port>` writes the box's
saved settings, so it starts as a blank Helrig Box. `--mode tonerelay` gives a plain ToneRelay box. See the script's header.

## The Helrig mode

A box can take on the names a Helrig user expects, so nobody has to learn a second product name. It is a saved setting, off by default
(the box keeps the ToneRelay names). `provision.sh` sets it at flashing; on a running box:

```bash
curl -X POST http://<box>/api/relay -H 'Content-Type: application/json' -d '{"mode":"helrig"}'
```

The box restarts. It then announces itself as `helrigbox.local` and "Helrig Box <id>", and its Wi-Fi setup network is
`HelrigBox-<id>`. The mode also turns relay-only on, and the box's front page is the setup page above in place of the ToneRelay
editor. The ports, the `/raw` protocol and the `_tonerelay._tcp` service are unchanged, so any other client of the relay still finds
the box. To go back, post `{"mode":"tonerelay"}`. Details are in section 3b of [raw-relay-protocol.md](raw-relay-protocol.md).

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
