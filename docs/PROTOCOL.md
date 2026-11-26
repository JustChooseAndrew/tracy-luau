# Wire protocol (v2)

How `Tracy.luau` talks to `tracy-roblox-bridge`. Every number is little-endian, which
is what Luau's `buffer` library writes.

```
client VM ──RemoteEvent "TracyRelay"──► server VM ──HTTP POST /ingest──► bridge ──► Tracy
            (client batches, clock sync)           (server batch + relayed client batches)
```

- **Server.** It posts to the bridge over HTTP: `POST /ingest` with
  `Content-Type: application/octet-stream`.
- **Clients.** Roblox doesn't allow HTTP from client scripts. A client sends its batches
  to the server over a RemoteEvent named `TracyRelay`, which the server creates under
  the `Tracy` ModuleScript. The server forwards them inside its own requests, so clients
  cost nothing from the server's HttpService budget.

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

**PLOT_CONFIG fields.**
- `format`: 0 Number, 1 Memory (bytes), 2 Percentage.
- `flags`: bit 0 step, bit 1 fill.

## Response

| Status | Body | Meaning |
|---|---|---|
| 204 | | every batch accepted |
| 200 | one line per batch that needs attention | see the table below |
| 400 | reason | the request couldn't be read at all: empty, bad magic or version, or a bad first header |
| 415 | | `Content-Type` isn't `application/octet-stream`. A web page can send a plain cross-site POST to localhost, but not one with this type, so this keeps browsers out |

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
- **Re-registration.** After any loss, Luau rebuilds its pending batch with every name
  and plot config at the front. That covers registrations that went down with the lost
  batch. When the bridge comes back after an outage, the server tells every client to
  do the same.
- **Server clock.** On a server session's first batch, the bridge sets
  `offset = bridgeNow − flushClock` and keeps it for the whole session. Durations are
  exactly what Luau measured. Absolute placement is off by the transit time, which is
  well under a millisecond on localhost.
- **Client clock sync.** A client pings the server 8 times over the relay:
  1. The client sends `"sync", c0`.
  2. The server answers `"sync", c0, s`.
  3. The client receives the answer at `c1`.

  The client then uses `clockOffset = s − (c0 + c1)/2`, taken from the ping with the
  lowest round trip. If that is within half a round trip of zero, the two VMs share a
  clock. Play Solo runs both in one process, so there it is exactly 0.
- **Client placement.** The bridge maps a relayed client batch with the server
  session's offset from the same request, plus the batch's `clockOffset`. Client and
  server tracks land on one timeline.
- **Tracks.** Each session is replayed on one bridge thread (a "lane"), which Tracy
  shows as one track: `Roblox Server`, `Roblox Client 1`, and so on. A new session
  takes over a lane of the same source once that lane's session sent `FINAL` or has
  been silent for 5 s, so repeated playtests reuse their tracks. The server sends
  `FINAL` for a client's session when that player leaves, or when the server shuts
  down.
- **Global names get a track prefix.** Plots and named frame sets are global in Tracy,
  so the bridge prefixes them with the track: `Server: Lua heap`, `Client 1: Physics`.
  The server's unnamed frames use Tracy's default frame set. A client's unnamed frames
  go to a set named after the client (`Client 1`).
- **Late batches.** Anything a session sends after `FINAL` is ignored.
- **Zero-length zones.** `os.clock()` ticks every 100 ns, so a zone can end on the tick
  it began. Tracy leaves zero-length zones out of its statistics. The bridge ends them
  1 ns after they begin, so they still count.
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

**How conversion works.**
1. The bridge starts a private Tracy client on a free port and points `tracy-capture`
   at it.
2. It replays each request at `replayStart + arrival` on a virtual clock. Tracks, clock
   offsets and lane reuse therefore come out exactly as they did live, as fast as the
   file can be read.
3. `replayStart` begins a little after the capture connects. The bridge pre-scans the
   recording for how far before its seal any batch's earliest event lies, and leaves
   that much room. Tracy drops zones that fall before a trace's start.

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

## Changes from the original plan

- **Timestamps are `f64` seconds, not `u64` nanoseconds.** Luau's `buffer` library has
  no 64-bit integer write. Splitting a value into two `u32`s would add math to every
  event. An `f64` of `os.clock()` is the same 8 bytes, costs nothing to produce, and
  keeps sub-nanosecond precision for any realistic uptime. The bridge does the
  conversion.
- **Added a batch header.** It carries the session, sequence, clocks, and length. The
  bridge needs them for loss detection, playtest boundaries, clock mapping, and several
  batches per request.
- **Clients relay through the server.** The plan's `SourceID` header byte is there, but
  HttpService only works on the server, so the server forwards client batches. It also
  assigns each client's number.
- **New events.** `0x06 FRAME_MARK_NAMED` (`FrameMark(name)`), `0x07 ZONE_BEGIN_COLOR`,
  `0x08 MESSAGE`, `0x09 PLOT_CONFIG`, and for imports `0x0A TRACK_NAME` and
  `0x0B ZONE_TEXT`.

## Status endpoint

`GET /status` returns JSON: bridge version, protocol, Tracy version, whether a viewer is
connected, and per-track counters (`label`, `source`, `session`, `batches`, `events`,
`gaps`, ...). The Studio plugin's connection light will read it (the plugin comes after
the core is done).
