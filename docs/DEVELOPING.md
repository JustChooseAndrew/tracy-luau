# Developing Tracy-Luau

## Building the bridge

Windows needs Visual Studio 2022 Build Tools and CMake:

```powershell
powershell -ExecutionPolicy Bypass -File bridge\build.ps1
```

macOS and Linux:

```sh
cmake -S bridge -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Tracy's client and cpp-httplib are vendored in `bridge/third_party/`.

## Layout

| Path | What |
|---|---|
| `src/Tracy/` | the Luau module, and `Capture` |
| `bridge/` | the bridge (C++17) |
| `tools/` | the MicroProfiler importer, `fetch_captures.py`, and the tests |
| `examples/`, `bench/` | example scripts and the overhead benchmark |
| `docs/` | the guide, the wire protocol, and benchmarks |

## Testing

```powershell
python tools/test_bridge.py --tracy-dir path\to\tracy-0.15.0
```

```powershell
python tools/test_fetch_captures.py
```

- **Without `--tracy-dir`,** only the HTTP checks run. That's what CI does.
- **To also check MicroProfiler conversion,** add `--lute PATH --gprx CAPTURE`.
- **Luau checks:** `stylua --check src examples bench` and `selene src examples bench`.

## Releasing

1. Bump the version in `src/Tracy/init.luau`, `bridge/CMakeLists.txt`, `wally.toml`
   and `pesde.toml`, and add it to `CHANGELOG.md`.
2. Push a `v<version>` tag. CI builds every platform and publishes the GitHub release.
3. Run `wally publish` and `pesde publish`.
