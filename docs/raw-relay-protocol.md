# Raw relay (`/raw`), protocol v0

Status: **implemented on the ESP32-P4 as a prototype.** Items marked *(planned)* are not implemented yet.
Written against ToneRelay `3656339`.

A mode in which the box carries the pedal's USB bulk transfers between one network client and a Helix/HX
pedal and does not interpret them. The client speaks the pedal's protocol itself; the box only moves bytes.

Tags: **[measured]** observed on a bench (ESP32-P4-NANO, HX Stomp and Helix LT, over Wi-Fi); **[read]** seen
in this repo's source.

## 1. Goals and non-goals

Goals
- A client on the LAN can use the pedal as if it were plugged in by USB, with its own protocol implementation.
- The box needs no knowledge of the pedal's protocol in this mode.
- Existing behaviour (web GUI, `/ws`, `/api/cmd`) is unchanged while the mode is off.

Non-goals: TLS, sharing the pedal between clients, protocol translation, any pedal logic on the box.

## 2. The "relay only" setting

Relay mode is a setting saved in NVS (key `relay_only`), **off by default**. It is read and changed with
`GET` / `POST /api/relay` (section 8).

| Setting | Behaviour |
|---|---|
| off | As before. A client that connects to `/raw` is told `{"t":"error","code":"relay_unavailable"}` and the box closes the connection. |
| on | The box does not open a session of its own with the pedal: no handshake, no keepalives, no retries. The pedal belongs to whichever client holds `/raw`. The box's own JSON ops that need a session find none open. |

Rationale: a second handshake on a session the pedal already holds is a known way to wedge it [read: the
existing code's comments]. Turn relay mode on before attaching the pedal: a session the box already has open
is dropped without a session close.

## 3. Connecting

- `ws://<host>/raw` on the existing HTTP server (port 80). One client at a time.
- Refusals arrive as a text message and a close, after the WebSocket upgrade (not as an HTTP status):
  `relay_unavailable` (relay mode is off), `busy` (another client holds the pedal),
  `box_session_open` (the box's own session already holds the pedal), `origin_not_allowed` (section 3a).
- The box sets `TCP_NODELAY` and TCP keepalive on the socket (section 6a). Clients should set `TCP_NODELAY`
  too: they send transfers in quick pairs (for example 20 then 28 bytes), and Nagle's algorithm can hold the
  second until the first is acknowledged. [The effect on this stack was not measured.]
- *(planned)* A per-box token, sent as a WebSocket subprotocol `tonerelay.v0.<token>`, for `/raw` and
  ideally `/ws` and `/api/cmd` too. Today nothing here is authenticated; use it on a trusted LAN.
- mDNS: the box advertises `_tonerelay._tcp` on port 80 next to `_http._tcp`, with TXT keys `proto` (the
  `proto` of the hello), `path` (`/raw`) and `id` (the last three bytes of the chip's base MAC, in hex). A firmware version key
  is not defined yet because the firmware has no version string.

### 3a. Origin

A browser sends an `Origin` header on every WebSocket upgrade, and a web page can open a WebSocket to any
address it can reach, so a page open on the same network could otherwise talk to the pedal through the visitor's
browser. `/raw` accepts an upgrade with no `Origin` (a native client) or with an `Origin` whose authority equals the `Host`
header (a page the box served itself). Any other `Origin` gets `{"t":"error","code":"origin_not_allowed"}` and a close.
This does not stop DNS rebinding (the rebound name appears in both headers), a non-browser client that sets
`Origin` on purpose, or anything else on the LAN; it is not authentication.

### 3b. Modes and names

The box has a saved `mode` that decides the names it announces. It is read at boot, so changing it restarts the box.

| Mode | Wi-Fi setup network | mDNS host name | mDNS instance | mDNS services |
|---|---|---|---|---|
| `tonerelay` (default) | `ToneRelay` | `tonerelay.local` | `ToneRelay` | `_http._tcp`, `_tonerelay._tcp` |
| `helrig` | `HelrigBox-<id>` | `helrigbox.local` | `Helrig Box <id>` | `_http._tcp`, `_tonerelay._tcp`, `_helrigbox._tcp` |

`<id>` is the last three bytes of the chip's base MAC, in hex, the same value as the `id` TXT key. The `helrig` mode also turns
relay-only on. Nothing else depends on the mode: the contract, the port (80) and the `/raw` protocol are the same in both, and
`_tonerelay._tcp` is advertised in every mode so a client that follows this contract can always find the box. The TXT keys on
the service records are `proto`, `path` (`/raw`), `id` and `mode`.

After a mode change other devices may keep listing the old names (mDNS lists and Wi-Fi scan lists are cached) for up to about an
hour. The old names no longer answer, so a client cannot be misdirected by them, and the lists clear by themselves. [Measured on
one Mac: the old Wi-Fi network name disappeared from its list within a few minutes, and the old mDNS instance stayed listed
80 s after the change although it no longer resolved.]

Two boxes in `helrig` mode on one network both ask for `helrigbox.local`. The instance names differ (they carry the id), so a
client that discovers by service should use the address in the record and not the host name. [Not tested: only one box was
available.]

In the `helrig` mode the setup network exists only while the box has no Wi-Fi of its own to use: a box with a saved network starts as a
station only and raises the setup network if that network is not joined within 40 s; 45 s after the box gets an address the
setup network goes off; if the Wi-Fi is lost for 40 s it comes back. The `tonerelay` mode keeps its setup network up as before. In
the `helrig` mode `GET /` is the box's own setup and status page (it uses the `wifi_scan`, `wifi_join` and `wifi_status`
operations of `/api/cmd`) and not the ToneRelay editor. Holding the BOOT button for 5 s forgets the Wi-Fi network and the hotspot
password in either mode and restarts the box; the mode is kept.

The mode is read and set with `GET` / `POST /api/relay` (section 8). The BLE name stays `ToneRelay` in every mode.

## 4. Messages

Binary frames carry USB transfers. Text frames carry JSON control messages.

**Client to box, binary:** exactly one frame is one bulk OUT transfer (endpoint 0x01); the box never merges or
splits. At most 4096 bytes [read: the OUT limit]; a larger frame gets `{"t":"error","code":"oversize"}` and
the connection is closed. A failed USB write gets `{"t":"error","code":"usb_error"}` and a close.

**Box to client, binary:** exactly one frame is one bulk IN transfer (endpoint 0x81). A transfer ends at a short
packet: the box joins full 512-byte packets until a short one arrives (defensive; the largest IN seen was 272
bytes [measured]).

**Box to client, text**
- `hello`, first, before any binary frame:
  `{"t":"hello","proto":0,"fw":"relay-proto","pid":"0x424a","vid":"0x0e41","product":"HELIX   ","serial":"   0000123","mps":512}`
  `product` and `serial` are the pedal's USB string descriptors as read when it enumerated (space padding
  included, as the pedal reports it). With no pedal attached, `pid` is `null` and the box closes the connection.
- `{"t":"pedal_detached"}`, then a close, when the pedal leaves the bus. A USB read that fails otherwise sends
  `{"t":"error","code":"usb_error"}` and closes.
- `{"t":"closing","reason":"client_silent"}`, then a close (section 6a).
- `{"t":"error","code":...}`: `relay_unavailable`, `busy`, `box_session_open`, `origin_not_allowed`, `oversize`, `usb_error`.
- *(planned)* `pedal_attached` for a pedal that arrives while a client is connected. Reserved codes: `usb_stall`,
  `no_pedal`, `internal`.

**Client to box, text:** `{"t":"ping"}` is answered with `{"t":"pong"}`; WebSocket ping frames are answered
with pong frames. Nothing else is defined.

## 5. Ordering and backpressure

- Frames are delivered in order in each direction.
- IN transfers are sent as they arrive. If the socket cannot take them, the relay task waits in the send and the
  USB layer parks its transfers so the pedal NAKs; the box does not drop IN bytes [read: dropped stream bytes
  wedge the pedal, per existing comments].
- OUT frames are written to the pedal in arrival order, one at a time, with a 2 s limit each. They are not acknowledged.

## 6. Session ownership

The box has no view of the pedal's session. It sends no keepalives and answers none of the pedal's traffic. Everything
the client sends, including a polite session close, goes to the pedal unchanged. When a client disconnects cleanly, the
box releases the USB interface and sends the pedal nothing of its own.

## 6a. Client liveness

The client must send at least one frame (a binary transfer, or the text `{"t":"ping"}`) every 5 seconds. A client's
own protocol keepalives satisfy this. After 6 seconds of silence the box sends `closing` / `client_silent`, closes
the connection and releases the pedal, so a client that vanished (Wi-Fi lost, laptop closed, app crashed) does not
hold it. TCP keepalive (probe after 5 s of idle, every 2 s, give up after 3) is a second line. [measured, 2026-10-22:
without this the box held the pedal for a dead client for about three minutes and the returning client could only be
told "busy".] The check runs between USB reads; a send that is blocked on a dead connection ends when TCP gives up.

## 7. Unclean disconnects

When a client vanishes the pedal may still believe a client is attached: the box releases the interface but cannot send
the pedal's session close (it knows nothing of the pedal's protocol).

- Implemented: the box releases the pedal when the client is dropped (section 6a). In the bench runs the next client's
  handshake succeeded after Wi-Fi loss and after a power loss of the box. A pedal left with a stale session can instead
  stall the next handshake; a client should say so and suggest power-cycling the pedal.
- *(planned)* A `stale_session` flag in the next `hello` when the pedal has not been seen to re-enumerate.
- *Considered, not done:* sending the session closes from the box. That needs protocol knowledge this design keeps out of
  the box.

## 8. `GET` / `POST /api/relay`

`GET /api/relay` returns, without opening the pedal:

```json
{"ok":true,"relay_only":true,"client":false,"proto":0,"mode":"tonerelay","restart":false,
 "pedal":{"present":true,"pid":"0x424a","vid":"0x0e41","product":"HELIX   ","serial":"   0000123"},
 "reset_reason":1,"prev_crumbs":[]}
```

`client` is true while a client holds the pedal. `mode` is the running mode (section 3b); `restart` is true in the reply to a request
that changed it. `reset_reason` is `esp_reset_reason()` and `prev_crumbs` are lifecycle
markers kept across a reset (diagnostics; they helped find a crash in this very code). `POST /api/relay` with
`{"on":true}` or `{"on":false}` changes the setting and returns the same JSON; turning it off disconnects a connected client.
`{"mode":"helrig"}` or `{"mode":"tonerelay"}` saves the mode and restarts the box about 0.8 s after the reply, which says
`"restart":true`; an unknown mode gets `400 {"ok":false,"error":"unknown_mode"}`, and a request while a client holds the pedal gets
`409 {"ok":false,"error":"busy"}` because a restart would drop a session the pedal still believes in. The busy check comes first, so while a client holds the pedal even an unknown mode gets `409`.

## 9. Versioning

`proto` in the `hello` is an integer; a client must refuse a major version it does not know. The mDNS TXT key
`proto` carries the same number.

## 10. Latency (informational)

With Wi-Fi power-save left at the ESP-IDF default (modem sleep), the WebSocket round trip to the box (text ping) was about 10 to 30 ms
under back-to-back requests and 75 to 140 ms median when requests were spaced 50 ms to 1 s apart, with multi-second outliers [measured].
The box now turns power-save off at boot (`esp_wifi_set_ps(WIFI_PS_NONE)`; it has mains power). With that, HTTP replies to `GET /api/relay`
from a Mac after a 5 s idle gap had a median of about 38 ms and a worst of 69 ms over 12 samples, against about 103 ms and 362 ms before;
back-to-back replies did not change (median about 25 ms) [measured, one box, one Mac]. A client that makes many small request/response
exchanges will still feel the delay of a poor Wi-Fi link.

The same setting decides whether the box hears mDNS questions sent to everyone: with power-save on it answered only questions sent
straight to its address, so a client that had not seen it announce itself (a new computer, a phone, `mdns-sd`) found nothing; with it
off, `mdns-sd` found the box in every run [measured].

## 11. Open points

1. Whether the token should also gate `/ws` and `/api/cmd`.
2. Whether `stale_session` can be set reliably (what the box can see on re-enumeration).
3. Browser clients: a page served over https cannot open plain `ws://` to a LAN address (mixed content; localhost
   is exempt). A browser app could use `/raw` only if served over http or if the box offers TLS.
4. Other targets (a Pi, other ESP32 boards): the contract assumes nothing specific to the P4.
