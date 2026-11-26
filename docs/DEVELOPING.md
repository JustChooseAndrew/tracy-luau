# Developing Tracy-Luau

## Building the bridge

- **Windows:** Visual Studio 2022 Build Tools (C++ workload) and CMake.

  ```powershell
  powershell -ExecutionPolicy Bypass -File bridge\build.ps1
  ```

- **macOS and Linux:**

  ```sh
  cmake -S bridge -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build
  ```

Tracy 0.15.0's client and cpp-httplib are vendored unmodified in `bridge/third_party/`,
with their licenses.

## Repository layout

| Path | What |
|---|---|
| `src/Tracy/init.luau` | the Luau module (server and client) |
| `src/Tracy/Capture.luau` | live-game MicroProfiler captures (needs LibMP) |
| `bridge/` | the bridge (C++17, CMake) |
| `docs/` | the guide, the wire protocol, and benchmarks |
| `examples/` | timed demos, the part fountain, and live captures |
| `bench/Overhead.server.luau` | overhead benchmark; run it in Play |
| `tools/microprofiler/gprx2tlrec.luau` | MicroProfiler dump → `.tlrec` importer (Lute + LibMP) |
| `tools/fetch_captures.py` | downloads `Tracy.Capture` captures from a DataStore (Open Cloud) |
| `tools/test_bridge.py` | bridge tests |
| `tools/test_fetch_captures.py` | `fetch_captures.py` against a mock of Open Cloud's DataStore API |
| `tools/fake_client.py` | sends synthetic batches, to test the bridge without Roblox |
| `.github/workflows/ci.yml` | lint, package check, bridge builds and tests on Windows, macOS and Linux; releases on `v*` tags |

## Testing

```powershell
python tools/test_bridge.py --tracy-dir path\to\tracy-0.15.0
```

```powershell
python tools/test_fetch_captures.py
```

`test_bridge.py` starts its own bridge on test ports (18080 and 18086), so it doesn't
disturb a bridge you already have running.
- **With the Tracy tools folder,** it also checks what Tracy recorded, and runs the
  recording tests.
- **With `--lute` and `--gprx CAPTURE`** (and LibMP next to the importer), it also
  checks MicroProfiler conversion, directly and through `/capture`.
- **Without them,** the HTTP, `/status` and `/capture` checks run. That's what CI does.

**Luau style.** Run `stylua --check src examples bench` and
`selene src examples bench`. The tool versions are in `aftman.toml`.

## Releasing

1. Bump the version in `src/Tracy/init.luau`, `bridge/CMakeLists.txt`, `wally.toml`
   and `pesde.toml`.
2. Add a `## <version>` section to `CHANGELOG.md`.
3. Push a `v<version>` tag. CI builds the bridge for every platform, and publishes a
   GitHub release with the zips and `Tracy.rbxm`. The release notes come from that
   changelog section.
4. Publish the packages yourself with `wally publish` and `pesde publish`.
