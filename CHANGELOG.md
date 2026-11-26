# Changelog

## 0.5.0

The first public release.

- **Studio streaming:** server and client zones, frames, plots and messages on one
  timeline in Tracy.
- **`Tracy.Wrap` and `Tracy.Instrument`:** zones without Begin/End pairs.
- **Leak-safe zones:** zones left open by a yield or an error are closed and flagged.
- **Live games:**
  - Zones go to the MicroProfiler.
  - `Tracy.Capture` saves server captures to a DataStore or a URL.
  - `tools/fetch_captures.py` downloads them.
- **`--record`:** saves a session as a `.tracy` file.
- **`--convert`:** turns recordings and MicroProfiler dumps into `.tracy` files.
- **Module attributes** (`Enabled`, `MicroProfiler`, `Url`, `BuiltinPlots`): used by
  the Studio plugin.
- **Release builds:** Windows, macOS and Linux bridges, plus Wally and Pesde packages.
