# Wire protocol (v2)

How `Tracy.luau` talks to `tracy-roblox-bridge`. Every number is little-endian, which
is what Luau's `buffer` library writes.

```
client VM ──RemoteEvent "TracyRelay"──► server VM ──HTTP POST /ingest──► bridge ──► Tracy
            (client batches, clock sync)           (server batch + relayed client batches)
```

- **The server** posts to the bridge: `POST /ingest`, with
  `Content-Type: application/octet-stream`.
- **Clients** can't make HTTP requests. They send batches to the server over the
  `TracyRelay` RemoteEvent, which the server creates under the `Tracy` module. The
  server forwards them inside its own requests.

## Request

One request carries one or more **batches** back to back. The server's own batch comes
first, even when it has no events, followed by any client batches it relayed.

## Batch header (36 bytes)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u32 | magic | `0x41554C54`, the bytes `TLUA` |
| 4 | u8 | version | `2` |
| 5 | u8 | source | `0` = server. 1–254 = client, numbered by the relaying server in join order. 255 = a named track (imports): the session's first batch opens with `TRACK_NAME` |
| 6 | u16 | flags | bit 0 `FINAL`: last batch of the session |
| 8 | u32 | session | random per Luau VM run |
| 12 | u32 | sequence | starts at 0, +1 per sealed batch, wraps at 2^32 |
| 16 | f64 | flushClock | `os.clock()` when the batch was sealed |
| 24 | f64 | clockOffset | seconds to add to this batch's times to get the relaying server's clock. Always 0 for the server's own batches |
| 32 | u32 | length | bytes of events after the header |

## Events

Events follow the header back to back, in the order they happened. `time` is the
`os.clock()` reading in seconds. String ids are scoped to a session. Colors are
`0xRRGGBB`, and 0 means the viewer's default.

| Op | Name | Layout after the opcode byte | Size |
|---|---|---|---|
| `0x01` | REGISTER_STRING | `id u16, len u16, len bytes of UTF-8` | 5 + len |
| `0x02` | ZONE_BEGIN | `id u16, time f64` | 11 |
| `0x03` | ZONE_END | `time f64` | 9 |
| `0x04` | FRAME_MARK | `time f64` | 9 |
| `0x05` | PLOT_VALUE | `id u16, time f64, value f64` | 19 |
| `0x06` | FRAME_MARK_NAMED | `id u16, time f64` | 11 |
| `0x07` | ZONE_BEGIN_COLOR | `id u16, time f64, color u32` | 15 |
| `0x08` | MESSAGE | `time f64, color u32, len u16, len bytes` | 15 + len |
| `0x09` | PLOT_CONFIG | `id u16, format u8, flags u8, color u32` | 9 |
| `0x0A` | TRACK_NAME | `len u16, len bytes` | 3 + len |
| `0x0B` | ZONE_TEXT | `len u16, len bytes`, text added to the innermost open zone | 3 + len |

The importer sends MicroProfiler labels as `ZONE_TEXT`. The module uses it to label a
zone it closed because the zone was still open at the end of a frame.

**PLOT_CONFIG fields.**
- `format`: 0 Number, 1 Memory (bytes), 2 Percentage.
- `flags`: bit 0 step, bit 1 fill.

## Response

| Status | Body | Meaning |
|---|---|---|
| 204 | | every batch accepted |
| 200 | one line per batch that needs attention | see the table below |
| 400 | reason | the request couldn't be read at all: empty, bad magic or version, or a bad first header |
| 415 | | `Content-Type` isn't `application/octet-stream`. Web pages can't send this type cross-site, so this keeps browsers out |

The 200 response lines:

| Line | Meaning |
|---|---|
| `resync <session>` | An event used an id this session never registered. The owner re-sends every registration and plot config at the front of its next batch. For a client session, the server passes the request on over the relay |
| `error <session> <reason>` | That batch was malformed. The bridge closed that session's open zones |
| `error - <reason>` | The framing broke partway through the request. Batches before that point still counted |

`<session>` is the session id as 8 hex digits.

## Rules both sides rely on

- **One request in flight per server.** Batches arrive in order, so the bridge never
  reorders. A client's RemoteEvents arrive in order too.
- **Gaps.** A sequence number that skips means batches were lost. The bridge closes
  every open zone on that track, so later `ZONE_END`s can't close the wrong zone. A
  `ZONE_END` with no open zone is dropped and counted (`droppedEnds` in `/status`).
- **Re-registration.** After any loss, the module puts every name and plot config at
  the front of its next batch. When the bridge comes back, the server tells every
  client to do the same.
- **Server clock.** On a server session's first batch, the bridge sets
  `offset = bridgeNow − flushClock` and keeps it for the whole session. Durations stay
  exactly as measured.
- **Client clock sync.** A client pings the server 8 times over the relay:
  1. The client sends `"sync", c0`.
  2. The server answers `"sync", c0, s`.
  3. The client receives the answer at `c1`.

  The client uses `clockOffset = s − (c0 + c1)/2` from the ping with the lowest round
  trip, or exactly 0 when that's within half a round trip of zero, as in Play Solo.
- **Client placement.** The bridge maps a relayed client batch with the server
  session's offset from the same request, plus the batch's `clockOffset`. Client and
  server tracks land on one timeline.
- **Tracks.** Each session gets one track: `Roblox Server`, `Roblox Client 1`, and so
  on. A new session reuses a track once the old one sent `FINAL` or went quiet for 5 s.
  The server sends `FINAL` for a client when that player leaves, or when it shuts down.
- **Global names get a track prefix.** Plots and named frame sets are global in Tracy,
  so the bridge prefixes them with the track: `Server: Lua heap`, `Client 1: Physics`.
  The server's unnamed frames use Tracy's default frame set. A client's unnamed frames
  go to a set named after the client (`Client 1`).
- **Late batches.** Anything a session sends after `FINAL` is ignored.
- **Zero-length zones.** Tracy leaves these out of its statistics, so the bridge ends
  them 1 ns after they begin.
- **Timestamp validation.** A `time`, `flushClock`, or `clockOffset` that is NaN,
  infinite, or outside ±1e8 s makes the batch malformed. A NaN or infinite plot value
  skips just that sample.
- **Named tracks.** A source-255 session takes the track its `TRACK_NAME` gives, such
  as `RBX Main` or `RBX Worker A`, and only reuses a track with that name. The first
  server track or named track created owns Tracy's default frame set.

## Recordings (`.tlrec`)

`--record` saves every request it receives. `--convert` replays them later.

```
file:   "TLUAREC1" | u32 protocol version | u32 reserved
record: u64 arrival ns since the first record | u32 length | request body
```

**How conversion works.** The bridge points `tracy-capture` at a private Tracy client,
then replays each request at `replayStart + arrival` on a virtual clock, so the result
matches what the live viewer would have shown. `replayStart` leaves room for the
earliest event, because Tracy drops zones from before a trace starts.

The MicroProfiler importer (`tools/microprofiler/gprx2tlrec.luau`) writes this same
format. It uses one named session per MicroProfiler thread and one request per frame.

## Relay messages (RemoteEvent `TracyRelay`)

| Direction | Arguments | Meaning |
|---|---|---|
| client → server | `"batch", buffer` | a sealed client batch. The server checks the header and writes the client's number into `source` |
| client → server | `"sync", c0` | clock sync ping |
| server → client | `"sync", c0, s` | clock sync answer |
| server → client | `"resync"` | re-send every registration |
| server → client | `"status", connected` | whether the server is reaching the bridge |

## Status endpoint

`GET /status` returns JSON with:
- the bridge, protocol and Tracy versions;
- whether a viewer is connected;
- per-track counters (`label`, `source`, `session`, `active`, `batches`, `events`,
  `gaps`, ...).

## Captures endpoint

`POST /capture` saves one MicroProfiler capture.

| Part | Content |
|---|---|
| `Content-Type` | `application/octet-stream`, or 415 (the same guard against web pages as `/ingest`) |
| `X-Tracy-Capture` | optional: the capture info as JSON, plain or base64-encoded. HttpService refuses `{` in headers, so `Tracy.Capture` sends base64 |
| body | the capture: any container LibMP reads |

**Responses.**
- **200** `saved <file>`. The capture goes to `--captures DIR` (default `captures/`) as
  `<Id>.gprx`, with the info as `<Id>.json`.
  - `<Id>` is the info's `Id` if it's a plain token (letters, digits, `-`, `_`), else a
    UTC timestamp.
  - A name that's already taken gets `-2`, `-3`, and so on.
- **400** for an empty body.

When Lute, the importer (with LibMP beside it) and `tracy-capture` are all available,
each capture is also converted to `<Id>.tracy` in the background. Pass
`--no-convert-captures` to turn that off.

## DataStore layout (`Tracy.Capture.DataStoreSink`)

| Key | Value (a Luau string) |
|---|---|
| `<Id>/1` ... `<Id>/N` | base64 of consecutive slices (2.4 MB each, at most) of the capture |
| `<Id>` | the capture info as JSON, plus `Parts` (N), `Format` (`libmp-compressed`) and `Version` (1). Written last, so its presence marks a complete capture |

`tools/fetch_captures.py` lists the keys through Open Cloud, reassembles each capture,
and checks its size against `Bytes`.
