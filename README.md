# StreamAudioPlugin (VST3)

<p align="center">
  <strong>Clean-room Windows x64 VST3 audio streaming plugin compatible with Cockos ReaStream (<code>MRSR</code> UDP)</strong><br/>
  Built for low-latency DAW-to-OBS audio routing (e.g., Universal Audio LUNA Main Output &rarr; OBS Studio) with pass-through studio monitoring.
</p>

<p align="center">
  <a href="#english">English</a> &bull;
  <a href="#portugu%C3%AAs">Português</a> &bull;
  <a href="https://github.com/ViniciusCostaGrillo/StreamAudioPlugin/releases">Releases</a>
</p>

---

<a id="english"></a>

## English

### Overview

**StreamAudioPlugin** is a native Windows x64 **VST3** (`Fx|Network`) plugin written in C++17 against the official **Steinberg VST3 SDK**. It implements the `MRSR` UDP audio streaming protocol on port `58710`, allowing VST3-only DAWs (such as **Universal Audio LUNA**) to stream real-time audio directly to **ReaStream (VST2)** inside **OBS Studio** — or between DAW tracks — without virtual audio cables or extra latency.

> **Clean-room implementation:** Written from scratch in C++17. No Cockos binaries, headers, or disassembled code are included or redistributed.

### Key Features

- **Plug-and-Play LUNA &rarr; OBS Routing:** Defaults to `Enabled`, `Mode = Send`, `Identifier = default`, `Destination = * local broadcast` (`127.255.255.255`), and `Channels = 2`.
- **Pass-Through Monitoring:** In `Send` mode, input audio passes straight through to your studio monitors unmodified while streaming over UDP.
- **Real-Time Safe Audio Thread:** Zero heap allocations, zero mutex locks, and zero blocking socket calls inside `IAudioProcessor::process()`.
- **Lock-Free Networking & Jitter Control:**
  - Process-wide WinSock2 singleton (`SO_REUSEADDR`, `SO_BROADCAST`) with `GetIpAddrTable` self-echo filtering that preserves `127.x` loopback communication.
  - Lock-free SPSC ring buffers, 2-block jitter prebuffer, 64-block overflow trimming, and stateful linear resampling (`Receive` mode) for mismatched sample rates.
- **Native Win32 Studio UI (`420x260`):** Flicker-free double-buffered GDI interface featuring `TX`/`RX`/`OFF` status pill, stereo `L`/`R` dBFS peak meters, and recent custom IP persistence in `%APPDATA%\StreamAudioPlugin\recent.ini`.

### Quick Start (LUNA DAW &rarr; OBS Studio)

1. **Install the VST3 Plugin**
   - Download `StreamAudioPlugin-v1.0.0-win64.zip` from [Releases](https://github.com/ViniciusCostaGrillo/StreamAudioPlugin/releases).
   - Extract `StreamAudioPlugin.vst3` into:
     ```text
     C:\Program Files\Common Files\VST3\StreamAudioPlugin.vst3
     ```
2. **Configure Your DAW (Sender)**
   - Insert **StreamAudioPlugin** on your **Main Output** bus (or any track/bus).
   - Keep the defaults: `Enabled` checked, `Mode: Send`, `Identifier: default`, `Destination: * local broadcast`, `Channels: 2`.
3. **Configure OBS Studio (Receiver)**
   - Add an **Audio Input Capture** (or audio source filter) in OBS Studio.
   - Add the **ReaStream (VST2)** plugin and set it to **`Receive audio/MIDI`** with **`Identifier: default`**.
   - If Windows Defender Firewall prompts on first run, allow Private Network access so UDP port `58710` can communicate locally.

### Building from Source (Windows x64)

**Requirements:**
- `clang++` (`llvm-mingw` UCRT x86_64)
- `git` & PowerShell

```powershell
# 1. Clone the repository and fetch Steinberg VST3 SDK
git clone https://github.com/ViniciusCostaGrillo/StreamAudioPlugin.git
cd StreamAudioPlugin
git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/steinbergmedia/vst3sdk.git vendor/vst3sdk

# 2. Build the VST3 bundle and run all 5 test suites
.\build.ps1 -Mode tests

# 3. Install to C:\Program Files\Common Files\VST3\ and verify the installed DLL
.\build.ps1 -Mode install
```

### Architecture & Repository Layout

| Path | Purpose |
| :--- | :--- |
| `src/Protocol.h` | `MRSR` 47-byte LE header codec, `<= 300` float chunking, IPv4 destination resolver |
| `src/RingBuffer.h` | Lock-free SPSC float ring buffer and `JitterChannelBuffer` (2-block prebuffer) |
| `src/Resampler.h` | Stateful linear resampler with continuous sub-sample phase across blocks |
| `src/UdpService.h/.cpp` | Ref-counted WinSock2 UDP singleton with `GetIpAddrTable` echo filter & SPSC queues |
| `src/ConfigStore.h` | `%APPDATA%\StreamAudioPlugin\recent.ini` MRU IPv4 storage (up to 10 entries) |
| `src/StreamAudioPlugin.h/.cpp` | Steinberg `SingleComponentEffect`, `'SAST'` state persistence, RT-safe `process()` |
| `src/Win32EditorView.h/.cpp` | Double-buffered Win32 `IPlugView` (`420x260`) dark studio UI & peak meters |
| `src/PluginFactory.cpp` / `exports.def` | VST3 factory registration and DLL entrypoints (`GetPluginFactory`, `InitDll`, `ExitDll`) |
| `tests/` | Unit, UDP loopback, VST3 host harness, and Win32 UI lifecycle test suites |

---

<a id="português"></a>

## Português

### Visão Geral

O **StreamAudioPlugin** é um plugin **VST3** nativo para Windows x64 (`Fx|Network`), escrito em C++17 com o **Steinberg VST3 SDK** oficial. Ele implementa o protocolo de streaming de áudio UDP `MRSR` na porta `58710`, permitindo que DAWs que aceitam apenas VST3 (como o **Universal Audio LUNA**) enviem áudio em tempo real diretamente para o **ReaStream (VST2)** no **OBS Studio** — ou entre canais da DAW — sem cabos virtuais de áudio e sem atraso adicional na monitoração.

> **Implementação clean-room:** Código escrito do zero em C++17. Nenhum binário, header ou código da Cockos é copiado ou redistribuído.

### Principais Recursos

- **Configuração Pronta para LUNA &rarr; OBS:** Já inicia com `Enabled` ativado, `Mode = Send`, `Identifier = default`, `Destination = * local broadcast` (`127.255.255.255`) e `Channels = 2`.
- **Monitoração Pass-Through:** No modo `Send`, o áudio de entrada continua passando limpo para os monitores enquanto é transmitido via UDP.
- **Thread de Áudio Segura em Tempo Real:** Zero alocações de heap, zero mutexes e zero chamadas bloqueantes de rede dentro de `IAudioProcessor::process()`.
- **Rede Lock-Free e Controle de Jitter:**
  - Singleton WinSock2 compartilhado entre instâncias (`SO_REUSEADDR`, `SO_BROADCAST`) com filtro `GetIpAddrTable` contra eco próprio, preservando o loopback `127.x`.
  - Ring buffers SPSC lock-free, pré-buffer de jitter de `2` blocos, descarte de excesso acima de `64` blocos e resampler linear contínuo (modo `Receive`) para diferenças de sample rate.
- **Interface Nativa Win32 (`420x260`):** Tema escuro de estúdio com double-buffering GDI (sem flicker), indicador de status (`TX`/`RX`/`OFF`), medidores estéreo `L`/`R` em dBFS e histórico de IPs em `%APPDATA%\StreamAudioPlugin\recent.ini`.

### Guia Rápido (LUNA DAW &rarr; OBS Studio)

1. **Instale o Plugin VST3**
   - Baixe `StreamAudioPlugin-v1.0.0-win64.zip` na página de [Releases](https://github.com/ViniciusCostaGrillo/StreamAudioPlugin/releases).
   - Extraia a pasta `StreamAudioPlugin.vst3` para:
     ```text
     C:\Program Files\Common Files\VST3\StreamAudioPlugin.vst3
     ```
2. **Configure sua DAW (Transmissor)**
   - Insira o **StreamAudioPlugin** no **Main Output** (ou na trilha/barramento desejado).
   - Mantenha os padrões: `Enabled` marcado, `Mode: Send`, `Identifier: default`, `Destination: * local broadcast`, `Channels: 2`.
3. **Configure o OBS Studio (Receptor)**
   - Adicione uma fonte de **Captura de Entrada de Áudio** (ou filtro de áudio) no OBS Studio.
   - Adicione o plugin **ReaStream (VST2)** em modo **`Receive audio/MIDI`** com o mesmo **`Identifier: default`**.
   - Se o Firewall do Windows solicitar permissão na primeira execução, autorize o acesso em Rede Privada para liberar a porta UDP `58710`.

### Compilando do Zero (Windows x64)

**Pré-requisitos:**
- `clang++` (`llvm-mingw` UCRT x86_64)
- `git` e PowerShell

```powershell
# 1. Clone o repositório e baixe o Steinberg VST3 SDK
git clone https://github.com/ViniciusCostaGrillo/StreamAudioPlugin.git
cd StreamAudioPlugin
git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/steinbergmedia/vst3sdk.git vendor/vst3sdk

# 2. Compile o bundle VST3 e execute todas as 5 suítes de testes
.\build.ps1 -Mode tests

# 3. Instale em C:\Program Files\Common Files\VST3\ e valide o binário instalado
.\build.ps1 -Mode install
```

### Estrutura do Projeto

| Caminho | Função |
| :--- | :--- |
| `src/Protocol.h` | Codec de pacotes `MRSR` (47 bytes LE), chunking (`<= 300` floats) e resolvedor IPv4 |
| `src/RingBuffer.h` | Ring buffer SPSC lock-free e `JitterChannelBuffer` (pré-buffer de 2 blocos) |
| `src/Resampler.h` | Resampler linear com continuidade de fase entre blocos de tamanhos variáveis |
| `src/UdpService.h/.cpp` | Singleton WinSock2 UDP com filtro `GetIpAddrTable` e filas SPSC sem alocação |
| `src/ConfigStore.h` | Persistência de até 10 IPs recentes em `%APPDATA%\StreamAudioPlugin\recent.ini` |
| `src/StreamAudioPlugin.h/.cpp` | `SingleComponentEffect` VST3, estado `'SAST'` v1 e `process()` tempo real |
| `src/Win32EditorView.h/.cpp` | Editor `IPlugView` Win32 (`420x260`) com double-buffering GDI e medidores L/R |
| `src/PluginFactory.cpp` / `exports.def` | Registro da factory VST3 e exports DLL (`GetPluginFactory`, `InitDll`, `ExitDll`) |
| `tests/` | Testes unitários, loopback UDP, host VST3 e ciclo de vida da UI Win32 |
