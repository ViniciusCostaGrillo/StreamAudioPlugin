# StreamAudioPlugin (VST3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a clean-room Windows x64 VST3 plugin (`StreamAudioPlugin.vst3`) compatible with Cockos ReaStream's `MRSR` UDP audio protocol so Universal Audio LUNA DAW can stream Main Output audio in real time to OBS Studio (ReaStream VST2) with pass-through monitoring.

**Architecture:** Clean-room C++17 VST3 `SingleComponentEffect` backed by Steinberg's official VST3 SDK (`vendor/vst3sdk/`), compiled directly with the installed `llvm-mingw` (`clang++`) toolchain. Real-time audio processing decouples from WinSock2 UDP I/O (`port 58710`, `SO_BROADCAST`, `SO_REUSEADDR`) via lock-free SPSC ring buffers, a linear resampler, and a process-wide singleton network service, paired with a native Win32 `IPlugView` editor UI.

**Tech Stack:** C++17, `clang++` (`llvm-mingw` UCRT x86_64), Steinberg VST3 SDK, WinSock2 (`ws2_32`), IP Helper API (`iphlpapi`), Win32/GDI (`user32`, `gdi32`, `ole32`, `uuid`), PowerShell (`build.ps1`).

**Spec:** [2026-11-12-stream-audio-plugin-design.md](file:///c:/OBS%20Stream%20Audio/docs/superpowers/specs/2026-11-12-stream-audio-plugin-design.md)

## Global Constraints

- Target OS & architecture: Windows 10/11 `x86_64` only.
- Compiler & SDK: `clang++` (`llvm-mingw`, C++17) against cloned `vendor/vst3sdk` without requiring CMake or MSVC.
- Wire protocol: Little-endian `"MRSR"` audio packets (`47`-byte header + planar IEEE-754 `float32` samples) on UDP port `58710` (`0xE556`), max `max(1, 300 / channels)` frames per datagram (`<= 1200` bytes payload).
- Audio thread safety: Zero heap allocations, zero blocking socket/select calls, and zero UI calls inside `IAudioProcessor::process()`.
- Pass-through invariant: Input audio is always copied cleanly to outputs in `Send` mode; in `Receive` mode, decoded streamed audio is summed onto outputs.
- Configuration & defaults: `Enabled = true`, `Mode = Send`, `Identifier = "default"` (max 31 ASCII chars + NUL), `Destination = "* local broadcast"` (`127.255.255.255`), `Audio Channels = 2` (range `1..8`); recent custom IPs stored in `%APPDATA%\StreamAudioPlugin\recent.ini` (never under `Program Files`).
- Bundle layout & naming: Plugin name `StreamAudioPlugin`, category `Fx|Network`, bundle `StreamAudioPlugin.vst3/Contents/x86_64-win/StreamAudioPlugin.vst3`.
- Clean-room rule: Zero Cockos binaries, headers, or disassembled code may be copied or redistributed.

## Review Focus

1. **Malformed or truncated UDP packets on port 58710** (`< 47` bytes, wrong magic, `packet_size != actual_bytes`, `channels == 0` or `> 8`, `data_bytes != frames * channels * 4`): receiver must reject safely without out-of-bounds reads (pinned in Task 2).
2. **Sample-rate mismatch or varying DAW block sizes** (e.g., sender at `44100` Hz and host at `48000`/`96000` Hz, or partial buffers): resampler and ring buffer must maintain continuity without clicks or overreads (pinned in Task 3).
3. **Rapid `Mode` (`Send` <-> `Receive`) or `Identifier` changes while audio is running**: must update atomically/safely without torn strings, race conditions, or stalling `process()` (pinned in Task 4).
4. **Multiple `StreamAudioPlugin` instances or coexisting ReaStream on localhost**: shared `UdpService` bind with `SO_REUSEADDR` must not fail with `WSAEADDRINUSE` and must filter non-loopback local NIC broadcasts via `GetIpAddrTable` while preserving `127.255.255.255` loopback (pinned in Task 4).
5. **Host scanner / edge-case VST3 calls** (`process()` with `numSamples == 0`, null `processContext`, `inputs == nullptr`, or truncated/corrupt `setState` stream): plugin must never crash the DAW host (pinned in Task 5).

---

## File Structure

- `.gitignore` — Excludes `vendor/`, `build/`, compiled binaries/objects.
- `build.ps1` — PowerShell build, test runner, `.vst3` bundle packager, and installer.
- `src/PluginIds.h` — Unique VST3 component/controller `FUID`s and plugin metadata constants.
- `src/Protocol.h` — Pure C++ `MRSR` packet encoding, decoding, validation, chunking, and IPv4 destination resolution.
- `src/RingBuffer.h` — Lock-free SPSC float ring buffer and 2-block prebuffer / 64-block overflow jitter buffer.
- `src/Resampler.h` — Stateful linear interpolation resampler for sample-rate conversion on receive.
- `src/ConfigStore.h` — `%APPDATA%\StreamAudioPlugin\recent.ini` read/write helper (max 10 recent IPs).
- `src/UdpService.h` / `src/UdpService.cpp` — Reference-counted WinSock2 UDP singleton (send worker, receive worker, `GetIpAddrTable` filter, 2s stale expiry).
- `src/StreamAudioPlugin.h` / `src/StreamAudioPlugin.cpp` — Steinberg `SingleComponentEffect` implementation (buses, parameters, real-time `process()`, `getState`/`setState`, `createView`).
- `src/Win32EditorView.h` / `src/Win32EditorView.cpp` — Native Win32 `IPlugView` UI (dark GDI layout, controls, L/R peak meters, TX/RX status).
- `src/PluginFactory.cpp` / `src/exports.def` — VST3 factory registration and DLL entrypoints (`GetPluginFactory`, `InitDll`, `ExitDll`).
- `tests/` — Unit tests (`test_protocol.cpp`, `test_dsp_buffers.cpp`, `test_udp_service.cpp`, `test_vst3_host.cpp`, `test_editor_view.cpp`).

---

### Task 1: Project Setup, Steinberg VST3 SDK Acquisition & Build Smoke Check

**Files:**
- Create: `.gitignore`
- Create: `src/PluginIds.h`
- Create: `build.ps1`
- Test: `tests/test_sdk_smoke.cpp`

**Interfaces:**
- Produces:
  - `StreamAudio::kProcessorUID` (`Steinberg::FUID`)
  - `StreamAudio::kPluginName` (`"StreamAudioPlugin"`)
  - `StreamAudio::kVendorName` (`"StreamAudio"`)
  - `StreamAudio::kVersionStr` (`"1.0.0"`)
  - `vendor/vst3sdk/` (checked-out Steinberg VST3 SDK with `base`, `pluginterfaces`, `public.sdk`)
  - `build.ps1` (`-Mode smoke|tests|plugin|install`)

- [ ] **Step 1: Initialize git repository and `.gitignore`**

Run in `c:\OBS Stream Audio`:
```powershell
git init
```
Create `.gitignore` ignoring `vendor/`, `build/`, `*.exe`, `*.o`, `*.dll`, `*.vst3`.

- [ ] **Step 2: Clone Steinberg VST3 SDK into `vendor/vst3sdk`**

Run:
```powershell
git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/steinbergmedia/vst3sdk.git vendor/vst3sdk
```
Verify `vendor/vst3sdk/pluginterfaces/vst/ivstcomponent.h` and `vendor/vst3sdk/public.sdk/source/vst/vstsinglecomponenteffect.h` exist.

- [ ] **Step 3: Write failing SDK smoke test `tests/test_sdk_smoke.cpp`**

Include `<pluginterfaces/vst/ivstaudioprocessor.h>`, `<public.sdk/source/vst/vstsinglecomponenteffect.h>`, and `"../src/PluginIds.h"`; assert `std::string(StreamAudio::kPluginName) == "StreamAudioPlugin"` and `StreamAudio::kProcessorUID.isValid()`.

- [ ] **Step 4: Run smoke test to verify it fails**

Run: `clang++ -std=c++17 -Ivendor/vst3sdk tests/test_sdk_smoke.cpp -o build/test_sdk_smoke.exe`
Expected: FAIL (`src/PluginIds.h` not found).

- [ ] **Step 5: Implement `src/PluginIds.h` and `build.ps1`**

In `src/PluginIds.h`, declare `kProcessorUID` (explicit unique 128-bit `DECLARE_CLASS_IID` / `Steinberg::FUID` bytes), `kPluginName = "StreamAudioPlugin"`, `kPluginCategory = "Fx|Network"`, `kVendorName = "StreamAudio"`, `kVersionStr = "1.0.0"`.
In `build.ps1`, support `-Mode` (`smoke`, `tests`, `plugin`, `install`) invoking `clang++ -std=c++17 -O2 -DRELEASE=1 -D_UNICODE -DUNICODE -Ivendor/vst3sdk -Isrc`.

- [ ] **Step 6: Run smoke test to verify it passes**

Run: `.\build.ps1 -Mode smoke`
Expected: Compiles and `build/test_sdk_smoke.exe` exits with code `0`.

- [ ] **Step 7: Commit**

```powershell
git add .gitignore src/PluginIds.h build.ps1 tests/test_sdk_smoke.cpp docs/
git commit -m "chore: initialize VST3 SDK, plugin IDs, and clang++ build script"
```

---

### Task 2: ReaStream `MRSR` Wire Protocol Codec & Chunking

**Files:**
- Create: `src/Protocol.h`
- Test: `tests/test_protocol.cpp`

**Interfaces:**
- Consumes: standard C++17 headers (`<cstdint>`, `<cstring>`, `<string_view>`, `<vector>`, `<algorithm>`)
- Produces:
  - `StreamAudio::Protocol::kPort = 58710`
  - `StreamAudio::Protocol::kHeaderSize = 47`
  - `StreamAudio::Protocol::kMaxAudioFloatsPerPacket = 300`
  - `StreamAudio::Protocol::kMaxIdentifierBytes = 32`
  - `struct StreamAudio::Protocol::DecodedAudioPacket { char identifier[32]; uint8_t channels; uint32_t sampleRate; uint32_t frameCount; const float* planarSamples; }`
  - `uint32_t StreamAudio::Protocol::maxFramesPerPacket(uint8_t channels)`
  - `size_t StreamAudio::Protocol::encodeAudioPacket(uint8_t* outBuf, size_t outCapacity, const char* identifier, uint8_t channels, uint32_t sampleRate, uint32_t frames, const float* const* channelPtrs, uint32_t frameOffset)`
  - `bool StreamAudio::Protocol::decodeAudioPacket(const uint8_t* data, size_t len, DecodedAudioPacket& out)`
  - `enum class StreamAudio::Protocol::DestinationKind { LocalBroadcast, GlobalBroadcast, CustomIp }`
  - `bool StreamAudio::Protocol::resolveDestination(std::string_view text, DestinationKind& kind, uint32_t& ipv4NetworkOrder)`

- [ ] **Step 1: Write failing tests in `tests/test_protocol.cpp`**

Test cases (with explicit assertions):
1. `test_encode_layout_matches_mrsr`: encode 2 channels x 4 frames at 48000 Hz with identifier `"obs"`; assert returned size `== 47 + 32 == 79`, bytes `0..3 == "MRSR"`, `uint32_t` at offset `4 == 79`, bytes `8..39 == "obs\0..."`, byte `40 == 2`, `uint32_t` at `41 == 48000`, `uint16_t` at `45 == 32`, and planar `float32` samples at `47..78` (`ch0[0..3]` then `ch1[0..3]`).
2. `test_chunk_frame_count`: `maxFramesPerPacket(2) == 150`, `maxFramesPerPacket(8) == 37`, `maxFramesPerPacket(0) == 1`.
3. `test_decode_rejects_malformed`: assert `decodeAudioPacket` returns `false` for `< 47` bytes, wrong magic `"mRSR"`/`"XXXX"`, `packet_size != len`, `channels == 0`, `channels > 8`, `data_bytes != (len - 47)`, `data_bytes % (channels * 4) != 0`.
4. `test_destination_resolution`: `"* local broadcast"` -> `LocalBroadcast` (`127.255.255.255`), `"* local"` -> `LocalBroadcast`, `"* broadcast"` -> `GlobalBroadcast` (`255.255.255.255`), `"192.168.1.50"` -> `CustomIp`, `"invalid-ip"` -> `false`.

- [ ] **Step 2: Run test to verify it fails**

Run: `clang++ -std=c++17 -Isrc tests/test_protocol.cpp -o build/test_protocol.exe`
Expected: FAIL (`src/Protocol.h` not found).

- [ ] **Step 3: Implement `src/Protocol.h`**

Header-only implementation using explicit `memcpy` little-endian reads/writes (avoiding unaligned struct UB on packed 47-byte header), strict bounds checks on `decodeAudioPacket`, and IPv4 dotted-quad parsing for `resolveDestination`.

- [ ] **Step 4: Run test to verify it passes**

Run: `clang++ -std=c++17 -Isrc tests/test_protocol.cpp -o build/test_protocol.exe; .\build\test_protocol.exe`
Expected: Exit code `0`, all 4 test cases pass.

- [ ] **Step 5: Commit**

```powershell
git add src/Protocol.h tests/test_protocol.cpp
git commit -m "feat: implement MRSR UDP audio packet codec and destination resolver"
```

---

### Task 3: Lock-Free SPSC Ring Buffer, Jitter Prebuffer & Linear Resampler

**Files:**
- Create: `src/RingBuffer.h`
- Create: `src/Resampler.h`
- Test: `tests/test_dsp_buffers.cpp`

**Interfaces:**
- Produces:
  - `template<size_t Capacity> class StreamAudio::SpscFloatRing` with `size_t push(const float* src, size_t count)`, `size_t pop(float* dst, size_t count)`, `size_t availableRead() const`, `size_t availableWrite() const`, `void discard(size_t count)`, `void reset()`
  - `class StreamAudio::JitterChannelBuffer` (wraps `SpscFloatRing<131072>`) with `void push(const float* src, size_t count)`, `size_t popForBlock(float* dst, size_t blockFrames)`, `void reset()` (enforces 2-block initial prebuffer and trims oldest samples when buffered frames exceed `64 * blockFrames`)
  - `class StreamAudio::LinearResampler` with `void reset(double srcRate, double dstRate)`, `size_t process(const float* in, size_t inFrames, float* out, size_t outFrames, size_t& inConsumed)`

- [ ] **Step 1: Write failing tests in `tests/test_dsp_buffers.cpp`**

Test cases:
1. `test_spsc_wraparound_and_order`: push/pop chunks across buffer wraparound boundary, assert exact float sample sequence and zero data loss.
2. `test_jitter_prebuffer_and_overflow_trim`: with `blockFrames = 128`, pushing `128` frames returns `0` popped (still in 2-block prebuffer); pushing another `128` (`256` total) allows popping `128` frames; pushing `> 64 * 128` frames trims excess down to `<= 64 * 128`.
3. `test_resampler_rate_conversion`: resample a 1 kHz sine wave from `44100` Hz to `48000` Hz across varying block sizes (`64`, `128`, `512`), assert smooth output length matches requested `outFrames` and peak/RMS error vs ideal 1 kHz sine at 48 kHz stays `< 0.05`.

- [ ] **Step 2: Run test to verify it fails**

Run: `clang++ -std=c++17 -Isrc tests/test_dsp_buffers.cpp -o build/test_dsp_buffers.exe`
Expected: FAIL (`src/RingBuffer.h` / `src/Resampler.h` not found).

- [ ] **Step 3: Implement `src/RingBuffer.h` and `src/Resampler.h`**

Use `std::atomic<size_t>` with acquire/release memory ordering in `SpscFloatRing`, preallocated fixed arrays (zero heap allocations on `push`/`pop`), and fractional phase tracking across calls in `LinearResampler`.

- [ ] **Step 4: Run test to verify it passes**

Run: `clang++ -std=c++17 -Isrc tests/test_dsp_buffers.cpp -o build/test_dsp_buffers.exe; .\build\test_dsp_buffers.exe`
Expected: Exit code `0`, all 3 test cases pass.

- [ ] **Step 5: Commit**

```powershell
git add src/RingBuffer.h src/Resampler.h tests/test_dsp_buffers.cpp
git commit -m "feat: add lock-free SPSC ring buffer, jitter prebuffer, and linear resampler"
```

---

### Task 4: WinSock2 `UdpService` Singleton & Recent-IP Config Store

**Files:**
- Create: `src/ConfigStore.h`
- Create: `src/UdpService.h`
- Create: `src/UdpService.cpp`
- Test: `tests/test_udp_service.cpp`

**Interfaces:**
- Consumes: `StreamAudio::Protocol` (`src/Protocol.h`)
- Produces:
  - `StreamAudio::ConfigStore::getIniPath()` -> `std::string` (`%APPDATA%\StreamAudioPlugin\recent.ini`)
  - `StreamAudio::ConfigStore::loadRecentIps()` -> `std::vector<std::string>` (up to 10 valid IPv4 strings)
  - `StreamAudio::ConfigStore::saveRecentIp(const std::string& ip)`
  - `struct StreamAudio::ReceivedAudioChunk { uint32_t sampleRate; uint8_t channels; uint32_t frames; std::vector<float> planarSamples; }` (or preallocated fixed-slot ring queue for audio thread consumption!)
  - **Real-time constraint interface:** `UdpService` exposes `ReceiverSlot` with preallocated SPSC packet queue (`tryPopReceivedPacket(ReceiverSlot*, ReceivedPacketView&)` — zero heap alloc on audio thread!) and `SenderSlot` with preallocated outgoing frame queue (`tryPushSendAudio(SenderSlot*, const char* id, uint32_t destIpNet, uint8_t ch, uint32_t sr, uint32_t frames, const float* const* ptrs)` — zero heap alloc on audio thread!).
  - `UdpService::instance().acquire()`, `release()`, `createReceiver(const char* identifier)`, `destroyReceiver(ReceiverSlot*)`, `setReceiverIdentifier(ReceiverSlot*, const char* identifier)`, `createSender()`, `destroySender(SenderSlot*)`, `isRxActive(ReceiverSlot*)`, `isTxActive(SenderSlot*)`, `setBindPortForTest(uint16_t port)` (allows isolated port testing without clashing with live port `58710`, while defaulting to `58710`).

- [ ] **Step 1: Write failing tests in `tests/test_udp_service.cpp`**

Test cases:
1. `test_config_store_roundtrip`: override base dir to temp dir, save 12 IPs (including 1 invalid `"bad"` and 1 duplicate), verify only the 10 most recent valid IPv4s persist in `recent.ini`.
2. `test_udp_loopback_send_and_receive_routing`: bind `UdpService` on an ephemeral test port (and also verify default port `58710` constant); create sender + two receivers (`"busA"`, `"busB"`); push 2-channel 256-frame audio block to `"busA"` targeting `127.0.0.1`; wait up to 500 ms; assert `"busA"` receiver pops matching 256 frames (split cleanly across `MRSR` chunks) and `"busB"` receives `0` frames.
3. `test_multi_acquire_release_and_rapid_id_change`: call `acquire()` twice, rapidly change `setReceiverIdentifier` under concurrent send traffic, then `release()` twice; assert zero crashes/deadlocks.

- [ ] **Step 2: Run test to verify it fails**

Run: `clang++ -std=c++17 -Isrc tests/test_udp_service.cpp -lws2_32 -liphlpapi -o build/test_udp_service.exe`
Expected: FAIL (`src/UdpService.h` / `src/ConfigStore.h` not found).

- [ ] **Step 3: Implement `src/ConfigStore.h`, `src/UdpService.h`, and `src/UdpService.cpp`**

Implementation notes:
- `UdpService` uses WinSock2 (`WSAStartup`, `socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)`, `SO_REUSEADDR`, `SO_BROADCAST`, non-blocking mode via `FIONBIO`), binds `INADDR_ANY:port`.
- Queries `GetIpAddrTable` to build the list of non-loopback local IPv4 addresses so `recvfrom` drops packets sourced from our own LAN NICs (`(addr & 0xFF) != 0x7F`) while accepting `127.x` loopback broadcasts.
- Sender thread waits on `CreateEventA` (signaled by `tryPushSendAudio` via `SetEvent`, waitable in O(1) without mutex contention on the audio thread) and transmits `MRSR` chunks via `sendto()`.
- Receiver thread polls `select()` with `50 ms` timeout, decodes `MRSR` via `Protocol::decodeAudioPacket`, and pushes valid packets into matching `ReceiverSlot`'s preallocated fixed-slot SPSC ring (`MAX_QUEUED_PACKETS = 256`, `MAX_PACKET_FLOATS = 300`, dropping oldest if full or > 2s stale).

- [ ] **Step 4: Run test to verify it passes**

Run: `clang++ -std=c++17 -Isrc tests/test_udp_service.cpp src/UdpService.cpp -lws2_32 -liphlpapi -o build/test_udp_service.exe; .\build\test_udp_service.exe`
Expected: Exit code `0`, all 3 test cases pass.

- [ ] **Step 5: Commit**

```powershell
git add src/ConfigStore.h src/UdpService.h src/UdpService.cpp tests/test_udp_service.cpp
git commit -m "feat: add WinSock2 UDP singleton service and recent IP config store"
```

---

### Task 5: VST3 Processor, Controller, State Persistence & DLL Exports

**Files:**
- Create: `src/StreamAudioPlugin.h`
- Create: `src/StreamAudioPlugin.cpp`
- Create: `src/PluginFactory.cpp`
- Create: `src/exports.def`
- Test: `tests/test_vst3_host.cpp`

**Interfaces:**
- Consumes: `PluginIds.h`, `Protocol.h`, `RingBuffer.h`, `Resampler.h`, `UdpService.h`
- Produces:
  - `class StreamAudio::StreamAudioPlugin : public Steinberg::Vst::SingleComponentEffect`
  - Parameter IDs: `kParamEnabled = 0`, `kParamMode = 1` (`0 = Receive`, `1 = Send`, default `1`), `kParamChannels = 2` (`1..8`, default `2`)
  - Non-parameter config fields (persisted in state & editable from UI): `identifier` (32B, `"default"`), `destination` (64B, `"* local broadcast"`)
  - Atomic telemetry read by UI: `peakL`, `peakR`, `txActive`, `rxActive`, `lastSampleRate`
  - `build/StreamAudioPlugin.vst3/Contents/x86_64-win/StreamAudioPlugin.vst3` exporting `GetPluginFactory`, `InitDll`, `ExitDll`

- [ ] **Step 1: Write failing VST3 host-load test `tests/test_vst3_host.cpp`**

Compile a standalone test harness that:
1. Loads `build/StreamAudioPlugin.vst3/Contents/x86_64-win/StreamAudioPlugin.vst3` via `LoadLibraryA`.
2. Resolves and calls `InitDll()`, `GetPluginFactory()`, creates `IComponent` via `kProcessorUID`, queries `IAudioProcessor` and `IEditController`.
3. Calls `initialize`, `setBusArrangements` (stereo in/out), `setupProcessing` (`48000` Hz, max `512` samples, `kSample32`), `setActive(true)`, and `process()` with a known sine wave -> verifies exact pass-through in `Send` mode.
4. Calls `process()` with edge cases: `numSamples == 0`, `processContext == nullptr`, `numInputs == 0 / numOutputs == 0` -> verifies `kResultOk` and no crash.
5. Calls `getState` -> creates second instance -> `setState` -> verifies `enabled`, `mode`, `channels`, `identifier`, `destination` round-trip; also passes truncated/garbage `setState` bytes -> verifies safe fallback without crash.
6. Calls `setActive(false)`, `terminate()`, `ExitDll()`.

- [ ] **Step 2: Run test to verify it fails**

Expected: `.vst3` binary does not exist yet.

- [ ] **Step 3: Implement `StreamAudioPlugin.h`, `StreamAudioPlugin.cpp`, `PluginFactory.cpp`, and `exports.def`**

Key details:
- Use `SingleComponentEffect` from `vendor/vst3sdk/public.sdk/source/vst/vstsinglecomponenteffect.cpp`.
- Register audio input & output buses (`SpeakerArr::kStereo` default, up to 8 channels accepted via `setBusArrangements`).
- `getState` / `setState` serialize a versioned binary header (`magic = 'SAST'`, `version = 1`, `enabled`, `mode`, `channels`, `identifier[32]`, `destination[64]`).
- `process()` handles parameter changes from `data.inputParameterChanges`, copies input -> output (clearing extra output channels if `numInputs < numOutputs`), updates L/R peak atomics, and either pushes audio to `UdpService::tryPushSendAudio` (`Send` mode) or pops/resamples queued packets via `JitterChannelBuffer` + `LinearResampler` and sums onto outputs (`Receive` mode).
- `createView("editor")` returns `nullptr` for now (wired in Task 6).
- `exports.def` exports `GetPluginFactory`, `InitDll`, `ExitDll` cleanly for MinGW DLL linking.
- Update `build.ps1` `-Mode plugin` to compile SDK sources + plugin sources into `build/StreamAudioPlugin.vst3/Contents/x86_64-win/StreamAudioPlugin.vst3`.

- [ ] **Step 4: Build `.vst3` and run `tests/test_vst3_host.cpp`**

Run: `.\build.ps1 -Mode plugin; clang++ -std=c++17 -Ivendor/vst3sdk -Isrc tests/test_vst3_host.cpp -lole32 -luuid -o build/test_vst3_host.exe; .\build\test_vst3_host.exe`
Expected: `.vst3` compiles cleanly with zero errors and `test_vst3_host.exe` exits with code `0`.

- [ ] **Step 5: Commit**

```powershell
git add src/StreamAudioPlugin.h src/StreamAudioPlugin.cpp src/PluginFactory.cpp src/exports.def tests/test_vst3_host.cpp build.ps1
git commit -m "feat: implement VST3 StreamAudioPlugin processor, state, and factory exports"
```

---

### Task 6: Native Win32 Editor UI (`IPlugView`), Bundle Packaging & Installation Verification

**Files:**
- Create: `src/Win32EditorView.h`
- Create: `src/Win32EditorView.cpp`
- Modify: `src/StreamAudioPlugin.cpp` (`createView`)
- Modify: `build.ps1` (include `Win32EditorView.cpp`, bundle copy/install to `C:\Program Files\Common Files\VST3\`)
- Test: `tests/test_editor_view.cpp`

**Interfaces:**
- Consumes: `StreamAudioPlugin` (config getters/setters, telemetry atomics, `performEdit` for automatable params), `ConfigStore`
- Produces:
  - `class StreamAudio::Win32EditorView : public Steinberg::CPluginView` (or `Steinberg::IPlugView`)
  - Embedded Win32 child window (`420x260`) supporting `Steinberg::kPlatformTypeHWND`

- [ ] **Step 1: Write failing UI lifecycle test in `tests/test_editor_view.cpp`**

Load `StreamAudioPlugin.vst3`, instantiate `IEditController`, call `createView("editor")`, assert non-null `IPlugView*`, assert `isPlatformTypeSupported(kPlatformTypeHWND) == kResultTrue`, call `getSize()` (assert `420x260`), create a hidden Win32 parent window (`CreateWindowExA`), call `attached(hwnd, kPlatformTypeHWND)` -> pump messages briefly (`30 ms` timer tick) -> call `removed()` -> destroy parent window -> assert clean teardown and zero crashes.

- [ ] **Step 2: Run test to verify it fails**

Run: `.\build.ps1 -Mode plugin` and `test_editor_view.exe`
Expected: FAIL (`createView("editor")` returns `nullptr`).

- [ ] **Step 3: Implement `Win32EditorView.h/.cpp` and wire `StreamAudioPlugin::createView()`**

UI layout (`420x260`, clean dark studio theme `#181A1F` background, `#242830` cards, `#E6EAF0` text, `#3B82F6` / `#10B981` accents):
- Header: `StreamAudioPlugin` + live status pill (`TX` green / `RX` blue / `OFF` muted).
- Row 1: `Enabled` checkbox + `Mode` radio/buttons (`Send` / `Receive`).
- Row 2: `Identifier` edit box (default `"default"`, live apply on change/Enter, max 31 chars).
- Row 3: `Destination` editable combobox (`* local broadcast`, `* broadcast`, + recent IPs from `ConfigStore::loadRecentIps()`, saving valid custom IPv4 to `ConfigStore::saveRecentIp()` on commit/blur).
- Row 4: `Channels` selector (`1`..`8`, disabled when in `Receive` mode just like ReaStream).
- Footer: Stereo `L` / `R` dBFS peak bar meters updated on a `33 ms` Win32 timer (`WM_TIMER`) using double-buffered GDI painting (`BitBlt`) to prevent flicker, plus `SetWindowLongPtrA` `GWLP_USERDATA` for per-instance state.

- [ ] **Step 4: Build full `.vst3`, run all unit + host + UI tests, and install bundle**

Run:
1. `.\build.ps1 -Mode tests` (runs `test_protocol`, `test_dsp_buffers`, `test_udp_service`, `test_vst3_host`, `test_editor_view`)
2. `.\build.ps1 -Mode install` (copies `build/StreamAudioPlugin.vst3` to `C:\Program Files\Common Files\VST3\StreamAudioPlugin.vst3` and verifies destination `.vst3` loads cleanly via `test_vst3_host.exe`!)
Expected: All 5 test suites pass with exit code `0`, and `C:\Program Files\Common Files\VST3\StreamAudioPlugin.vst3\Contents\x86_64-win\StreamAudioPlugin.vst3` is verified present and loadable.

- [ ] **Step 5: Commit**

```powershell
git add src/Win32EditorView.h src/Win32EditorView.cpp src/StreamAudioPlugin.cpp build.ps1 tests/test_editor_view.cpp
git commit -m "feat: add Win32 IPlugView editor UI, package VST3 bundle, and verify install"
```
