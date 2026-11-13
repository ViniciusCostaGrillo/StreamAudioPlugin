# StreamAudioPlugin (VST3) — Design Document

**Date:** 2026-11-12  
**Workspace:** `c:\OBS Stream Audio`  
**Target Platform:** Windows 11 / Windows 10 (`x86_64`)  
**Approach Selected:** Option 1 — Clean-room C++17 VST3 plugin using official Steinberg VST3 SDK + lightweight native Win32 UI (`IPlugView`), compiled with installed `llvm-mingw` (`clang++`).

---

## 1. Intent & Why

The user records cover performances using **Universal Audio LUNA DAW** (Windows) alongside webcam video in **OBS Studio**. Currently, capturing desktop/system audio in OBS introduces noticeable latency/desync relative to the webcam. **ReaStream (VST2)** solves this by streaming DAW audio directly into OBS Studio over local UDP with low latency, **but LUNA DAW only supports VST3 plugins** (while OBS Studio natively loads VST2 plugins such as ReaStream).

**StreamAudioPlugin** bridges this gap cleanly: it is an original, clean-room VST3 plugin (`StreamAudioPlugin.vst3`) inserted on LUNA's **Main Output** that speaks the **ReaStream UDP audio protocol (`MRSR`, port 58710)** so **ReaStream (VST2) in OBS Studio** receives LUNA's audio stream directly over loopback with minimal latency, while passing audio through unchanged to the user's studio monitors.

---

## 2. Goals, Non-Goals, and Anti-Goals

### Goals
1. **LUNA DAW Compatibility (VST3):** Conform strictly to the Steinberg VST3 specification (`IPluginFactory`, `IComponent`, `IAudioProcessor`, `IEditController`, `IPlugView`) as an `Fx|Network` plugin named `StreamAudioPlugin`, packaged in standard Windows VST3 bundle layout (`StreamAudioPlugin.vst3/Contents/x86_64-win/StreamAudioPlugin.vst3`).
2. **OBS / ReaStream Interoperability:** Full wire-level compatibility with Cockos ReaStream `MRSR` UDP audio packets on UDP port `58710` (`0xE556`), verified against observed protocol layout from `reastream-standalone.dll`.
3. **Zero-Interference Audio Pass-Through:** In `Send` mode, copy all input channels cleanly to output channels without altering gain/phase so Main Output monitoring in LUNA is unaffected. In `Receive` mode, sum incoming streamed audio onto the plugin outputs (with pass-through preserved).
4. **Both Send & Receive Modes:** Support both `Send` (primary LUNA -> OBS workflow) and `Receive` (useful for DAW-to-DAW or loopback testing), switchable in UI and automatable.
5. **Real-Time Safe Audio Thread:** No heap allocations, blocking I/O, or UI calls inside `process()`. Audio-to-network handoff uses preallocated lock-free SPSC ring buffers and a dedicated background UDP thread.
6. **Clean Native Win32 Editor UI:** Compact, DPI-aware embedded Win32 editor (`420x260` logical px) displaying:
   - `Enabled` toggle
   - `Mode`: `Send` / `Receive`
   - `Identifier` text field (max 31 ASCII chars + null, default `"default"`)
   - `Destination` selector/edit: `* local broadcast` (`127.255.255.255`), `* broadcast` (`255.255.255.255`), or custom IPv4 (recent custom IPs saved in `%APPDATA%\StreamAudioPlugin\recent.ini`)
   - `Audio Channels`: `1`–`8` (default `2`, stereo)
   - Real-time `L/R` peak level meter + connection/packet activity indicator.
7. **State Persistence:** Save/restore plugin state (`enabled`, `mode`, `identifier`, `destination`, `channels`) via VST3 `getState` / `setState` so LUNA sessions recall configuration automatically.

### Non-Goals (v1)
- **MIDI over UDP (`mRSR` packets):** Not required for LUNA Main Output -> OBS Audio Input Capture.
- **macOS / Linux / 32-bit x86 builds.**
- **Modifying, patching, or redistributing any Cockos ReaPlugs binaries or code.**

### Anti-Goals
- Blocking sockets or `malloc`/`new` on the DAW audio callback thread (would cause audio dropouts/clicks in LUNA).
- Writing config files inside `C:\Program Files\` (fails without elevation; user recent IPs belong in `%APPDATA%\StreamAudioPlugin\`).
- Requiring MSVC or CMake installation when `llvm-mingw` (`clang++`) is already present on the machine.

---

## 3. Observed Protocol Specification (from REA + Ghidra Analysis)

All multi-byte integer/float fields are **little-endian**.

### Audio Packet (`MRSR`) — Header (47 bytes) + Planar Float32 Payload
| Offset | Size | Type | Value / Meaning |
|---|---|---|---|
| `0x00` | 4 | `char[4]` | Magic `"MRSR"` (`0x5253524d` LE) |
| `0x04` | 4 | `uint32_t` | Total packet size in bytes (`47 + data_bytes`) |
| `0x08` | 32 | `char[32]` | Stream identifier (null-padded/terminated ASCII, e.g. `"default"`) |
| `0x28` | 1 | `uint8_t` | Channel count (`1..N`) |
| `0x29` | 4 | `uint32_t` | Sample rate in Hz (e.g., `44100`, `48000`, `96000`) |
| `0x2D` | 2 | `uint16_t` | Audio payload size in bytes (`frames * channels * 4`) |
| `0x2F` | `data_bytes` | `float32[]` | Non-interleaved (planar) IEEE-754 float32 samples: all `frames` of ch0, then ch1, ... |

### Transmission & Reception Rules
- **UDP Port:** `58710` (`0xE556`).
- **MTU Chunking:** Outgoing audio blocks are split so each packet carries at most `max(1, 300 / channels)` frames (`<= 1200` bytes of audio payload per UDP datagram, e.g. 150 stereo frames = ~3.1 ms at 48 kHz).
- **Socket Setup:**
  - `SO_BROADCAST` enabled for `* local broadcast` (`127.255.255.255`) and `* broadcast` (`255.255.255.255`).
  - Receiver binds UDP port `58710` with `SO_REUSEADDR` (shared singleton receiver dispatcher across plugin instances in the same process, matching ReaStream's shared `UDPCom` + `GetIpAddrTable` design so multiple instances don't conflict on bind and non-loopback local NIC broadcasts don't loop back).
- **Jitter & Resampling (Receive Mode):**
  - Per-channel ring buffer with 2-block prebuffer before starting playback to absorb UDP jitter, and overflow trimming when buffered frames exceed `64 * blockSize`.
  - Linear interpolation resampler when sender `sample_rate` differs from DAW host `sampleRate`.
  - Stale/unconsumed queued packets older than 2 seconds are discarded.

---

## 4. Architecture & Design Decisions (Our Calls)

Since this is a greenfield project, the following engineering calls are made (and can be overridden by the user):

1. **Steinberg VST3 SDK Source Acquisition (`Our Call`):** Clone `https://github.com/steinbergmedia/vst3sdk.git` (recursive submodules for `base`, `pluginterfaces`, `public.sdk`) into `vendor/vst3sdk/` (MIT/GPLv3 dual-licensed; using MIT/Steinberg SDK headers + base classes cleanly).
2. **Build Toolchain (`Our Call`):** Compile directly with `clang++` (`llvm-mingw`, UCRT x86_64) via a clean `build.ps1` script (and `Makefile`/compile flags) linking `-lws2_32 -liphlpapi -lole32 -luuid -lgdi32 -luser32`, avoiding the need to install CMake or multi-GB MSVC Build Tools. Output `.vst3` bundle to `build/StreamAudioPlugin.vst3` and install into `C:\Program Files\Common Files\VST3\StreamAudioPlugin.vst3` (with fallback instructions/elevation prompt if Program Files write is denied).
3. **VST3 Component Model (`Our Call`):** Use `SingleComponentEffect` (combines `AudioEffect` + `EditController` in one class, standard in Steinberg VST3 SDK for utility/FX plugins without complex preset-only controller separation) plus a custom `IPlugView` subclass (`Win32EditorView`) using Win32 child window + GDI double-buffered drawing.
4. **Bus Configuration (`Our Call`):** Default to **Stereo (`SpeakerArr::kStereo`)** input and output buses (1 main audio input bus, 1 main audio output bus), with support for up to 8 channels if negotiated by the host (`Audio Channels` parameter controls how many channels are transmitted over UDP, `1..8`, default `2`).
5. **Default Plugin State (`Our Call`):** Default `Enabled = true`, `Mode = Send`, `Identifier = "default"`, `Destination = "* local broadcast"`, `Audio Channels = 2`. Reason: inserting the plugin on LUNA's Main Output should immediately work with OBS ReaStream (`Receive`, `"default"`) without extra clicks.
6. **Thread-Safe Audio/Network Decoupling (`Our Call`):**
   - **Send path:** Audio thread pushes planar floats into a preallocated lock-free SPSC ring buffer and signals an OS event (`SetEvent`) or atomic flag; a dedicated sender worker thread drains chunks and calls `sendto()` (non-blocking), keeping `process()` 100% real-time safe.
   - **Receive path:** Process-wide singleton `UdpService` (reference-counted on plugin initialize/terminate) runs a background `select()`/`recvfrom()` thread on port `58710`, filters local non-loopback interface IPs via `GetIpAddrTable`, and routes incoming `MRSR` packets by `identifier` into each receiving instance's lock-free ring buffer.
   - **UI <-> Processor sync:** Atomics for meters/activity/status and a mutex-protected `ConfigSnapshot` copied to an atomic/double-buffered config slot so UI edits (`Identifier`, `Destination`, `Mode`) never block the audio thread.
7. **Automated Verification (`Our Call`):**
   - Unit tests compiled with `clang++` testing: `MRSR` packet encode/decode byte layout (matching exact 47-byte header offsets), multi-chunk fragmentation (`300 / channels`), ring-buffer wraparound/prebuffer, resampler ratio accuracy, and live localhost UDP loopback send->receive round-trip.
   - VST3 binary verification: load `StreamAudioPlugin.vst3` via a minimal C++ VST3 host test harness (calling `InitDll`/`GetPluginFactory`, instantiating `IComponent`/`IAudioProcessor`/`IEditController`, calling `initialize`, `setupProcessing`, `setActive`, `process`, `getState`/`setState`, and `createView`) + Steinberg `validator` if buildable, verifying zero crashes and byte-accurate pass-through.

---

## 5. Component Breakdown (What's Left to the Builder)

- `vendor/vst3sdk/` — Steinberg VST3 SDK (`pluginterfaces`, `base`, `public.sdk`).
- `src/Protocol.h` — `MRSR` header packing/unpacking, chunking constants (`PORT = 58710`, `MAX_AUDIO_FLOATS_PER_PKT = 300`, `MAGIC = "MRSR"`).
- `src/RingBuffer.h` — Lock-free SPSC float ring buffer + prebuffer state machine.
- `src/UdpService.h/.cpp` — WinSock2 singleton UDP socket manager (`58710`, `SO_BROADCAST`, `SO_REUSEADDR`, `GetIpAddrTable` self-IP filter, receive thread, sender thread).
- `src/PluginIds.h` — Unique generated 128-bit `FUID`s for `StreamAudioPlugin` processor/controller, version strings, vendor metadata.
- `src/StreamAudioPlugin.h/.cpp` — `SingleComponentEffect` implementation (audio buses, `process()`, pass-through, parameters, `getState`/`setState`, `createView`).
- `src/Win32EditorView.h/.cpp` — Native Win32 `IPlugView` child window UI (dark studio aesthetic, controls for Enable, Mode, Identifier, Destination combo + recent history in `%APPDATA%`, Channels, L/R peak meters, live status).
- `src/PluginFactory.cpp` — VST3 `GetPluginFactory` + `InitDll` / `ExitDll` exports + `.def` file if needed for MinGW.
- `tests/` — Standalone C++ unit & VST3 host-load verification tests.
- `build.ps1` — Reproducible build, test, bundle packaging, and optional install script.
