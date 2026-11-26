# Changelog

## 0.5.0

The first public release. The module and the bridge share this version.

### Live games
- **Zones go to Roblox's MicroProfiler outside Studio.** `ZoneBegin`/`ZoneEnd` become
  `debug.profilebegin`/`debug.profileend`, so your zones show up in MicroProfiler views
  and captures of live servers and clients. `MICROPROFILER = "Live" | "Always" | "Off"`
  (or a `MicroProfiler` attribute) controls it.
- **`Tracy.Capture`.** Captures the last frames of the MicroProfiler on server hitches,
  or on demand. Sinks: `DataStoreSink` (nothing to host), `HttpSink`, `BridgeSink`
  (Studio), or your own. It sets the history size and memory budget, waits out the
  warm-up, and checks each capture for complete frames, retrying if there are none.
  Needs LibMP.
- **`tools/fetch_captures.py`.** Downloads DataStore captures through Open Cloud, checks
  their parts and sizes, and can convert and delete them. The API key comes from an
  environment variable.

### Bridge
- **`--convert` takes MicroProfiler captures.** `.gprx` files, HTML dumps, and
  `Tracy.Capture` files convert in one step, with the bridge running the Lute importer
  itself (`--lute`, `--importer`).
- **`POST /capture`.** Saves captures sent from Studio and converts them in the
  background (`--captures DIR`, `--no-convert-captures`).
- **Release builds for Windows, macOS (universal) and Linux.** The Windows zip bundles
  the matching Tracy 0.15.0 viewer and `tracy-capture`.

### Module
- **`Tracy.Wrap(name, fn)` and `Tracy.Instrument(table, prefix)`** add zones without
  Begin/End pairs. Arguments and results pass through. Outside Studio with mirroring
  off, `Wrap` returns your function unchanged.
- **Leak-safe zones.** A zone still open at the end of a frame (it yielded or errored)
  is closed and labeled in the viewer, and warned about once by name. Its late
  `ZoneEnd` is ignored. `GetStats()` adds `ZonesLeaked` and `StrayEnds`.
- **Attributes on the ModuleScript:** `Enabled` overrides `ENABLED`, and `Url` and `BuiltinPlots`
  set `Configure`'s defaults.
- **The module is now a folder** (`src/Tracy/init.luau` plus `Capture.luau`).

### Importer
- **Clear errors for empty captures.** A capture with no complete frames, or with frames
  but no scopes, fails with the likely cause instead of producing an empty trace.
- **`--quiet`.**

### Project
- **MIT license.**
- **Wally (`justchooseandrew/tracy-luau`) and Pesde (`justchooseandrew/tracy_luau`)
  manifests.**
- **`aftman.toml`, StyLua and selene configs, and CI.**

## 0.4.0 and earlier

Developed before the first release: streaming from Studio (phase 1), the binary
protocol and batching (2), plots, messages, colors and client tracks (3), and
`--record` and the MicroProfiler importer (4).
