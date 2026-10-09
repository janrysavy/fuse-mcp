# Fuse MCP integration

This fork preserves the original emulator while collecting the Windows JSON-RPC patches used for reproducible Spectrum analysis. SourceForge remains canonical upstream. No upstream pull request is being proposed.

The reproducible integration branch is `codex/fuse-project`, based on upstream `4094b14575b0aa9df4f211c2a34c0ae87f104c32`. Feature branches are intentionally stacked: `codex/json-rpc` is the foundation, followed by `codex/trace-inspection`, `codex/png-inspection`, `codex/audio-inspection`, and `codex/visible-presentation`. Each tip contains its prerequisite commits; compare it with its named prerequisite to review one feature. `codex/windows-test-paths` and `codex/fix-dat-line-endings` start independently from upstream. The integration branch merges each feature explicitly. `codex/mcp-documentation` adds this guide and distributes the adapter headers and MIT-licensed nlohmann/json 3.12.0.

## Build and launch

Use the ordinary MSYS2 UCRT64 Autotools build, a compatible libspectrum installation, C++17, Winsock2 and BCrypt. This adapter currently targets Windows. For headless automation configure `--without-win32 --with-null-ui --without-libxml2 --without-libpng --without-joystick --disable-sdl2 --with-audio-driver=null`. Configure `--with-win32` for an explicitly requested visible session. The same adapter compiles both modes; UI-only code is excluded under `UI_NULL`.

Launch `fuse.exe --machine 48 --no-sound --no-autosave-settings --speed 10000 --rpc-port 2304`. The native guest starts paused. RPC is bounded JSON-RPC 2.0 over TCP JSON-lines, bound to `127.0.0.1`. Query `agent.capabilities` for the endpoint list and limits. This is a native RPC endpoint; an MCP client needs a separate stdio bridge to these methods.

```json
{"jsonrpc":"2.0","id":1,"method":"agent.capabilities","params":{}}
```

All guest access occurs on Fuse's emulation thread at instruction boundaries. Transport threads own sockets and queues. Guarded writes require the caller's expected revision and register values. Passive inspection preserves physical CPU/ULA clocks; host speed changes only pacing.

## Observation features

`trace.start`, `trace.stop`, `trace.status` and `trace.read` provide bounded instruction records, memory writes and per-PC counts/T-states/contention. Device-event cycles are reported separately. Read pages while paused. The original CORETEST execution path remains unchanged.

`video.export_png` exports a native 256x192 RGBA PNG from `screen_ram` or the existing `ula_raster`. Screen-RAM projection accepts an explicit `flash_phase`; raster export does not. The palette is Fuse's 192/255 RGB palette. PNG and RGBA hashes support exact comparisons without scaling. Logical screen-RAM equality alone does not certify physical FLASH/raster timing.

`audio.start`, `audio.stop`, `audio.status` and `audio.read` capture the native output mixer/speaker filters and physical ULA edges without requiring a sound card. PCM includes complete raster frames and starts with fresh filter history. Capture uses the guest processor frequency at all host pacing percentages. Buffers are bounded and report dropped records. No synthetic filter is applied to make another machine sound more similar.

Visible builds also support `video.present` for a paused screen-RAM projection and keep the native event loop responsive while paused. Headless builds omit this operation. Presentation does not run the guest and is not campaign evidence.

## Licensing and payloads

The adapter is GPL-2.0-or-later. `json.hpp` preserves nlohmann/json's MIT license. These branches add source and documentation only: no game binaries, captures, proprietary payloads, DLLs or executables. Existing upstream files and their licenses remain part of the original Git history.
