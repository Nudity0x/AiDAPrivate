# AiDA

> **THIS REPOSITORY IS ACTIVELY MAINTAINED.** Development is live, commits land frequently, and the project is under very active construction (a full Qt port of the standalone IDE is currently in flight). **Contributors are welcome** — feel free to open issues, start discussions, and submit pull requests. If you want to help, jump in: bug reports, feature ideas, documentation, tests, and code are all appreciated. If you plan a large change, open an issue first so we can coordinate, because big refactors land often and we don't want you rebasing onto a moving floor.

![Maintenance](https://img.shields.io/badge/maintenance-active-brightgreen)
![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-blue)
![Qt](https://img.shields.io/badge/Qt-6.8.3%20msvc2022__64-41CD52)
![C++](https://img.shields.io/badge/C%2B%2B-17%20%2F%2020-00599C)
![Build](https://img.shields.io/badge/build-CMake%203.25%2B%20%2B%20Ninja%20%2B%20MSVC%202022-critical)

---

## Table of Contents

- [What is AiDA?](#what-is-aida)
- [Feature Overview](#feature-overview)
- [Repository Layout](#repository-layout)
- [Building From Source](#building-from-source)
  - [Prerequisites](#prerequisites)
  - [Qt 6.8.3 — Exact Version and Setup](#qt-683--exact-version-and-setup)
  - [Vendored Dependencies (In This Repo)](#vendored-dependencies-in-this-repo)
  - [Provisioned Dependencies (.deps)](#provisioned-dependencies-deps)
  - [Build Commands](#build-commands)
  - [Kernel Driver (WhosWho + WindMapper)](#kernel-driver-whoswho--windmapper)
  - [IDA Pro Plugin](#ida-pro-plugin)
- [Running AiDA](#running-aida)
- [MCP Servers and Tooling](#mcp-servers-and-tooling)
- [Testing](#testing)
- [Privacy and Security Model](#privacy-and-security-model)
- [Contributing](#contributing)
- [Responsible Use](#responsible-use)
- [Third-Party Notices and Acknowledgements](#third-party-notices-and-acknowledgements)

---

## What is AiDA?

AiDA is a Windows reverse-engineering toolkit that ships in three cooperating forms:

1. **AiDA Standalone** (`AiDAStandalone.exe`) — a full desktop IDE for reverse engineering: disassembly, decompilation, debugging, memory scanning, network analysis, AI-assisted chat, and an in-app Test Lab. It boots straight into the IDE — no license screen, no activation, no network requirement.
2. **AiDA IDA Pro plugin** — embeds AiDA's analysis surface inside IDA Pro and exposes it over a localhost MCP server, so AI agents and external tools can drive IDA analysis, decompilation, renaming, patching, typing, commenting, and more.
3. **WhosWho kernel driver + WindMapper loader** — a functional KMDF driver providing the heavy lifting: physical/virtual memory access, DTB walking, process/thread introspection, hardware breakpoints, remote calls, network capture/injection (WFP), and analysis-target sandboxing.

There is **no license system, no server, no anti-tamper, no protector, no string obfuscation, and no telemetry** anywhere in this codebase. What you clone is what runs.

## Feature Overview

**Standalone IDE**
- Multi-tab reversing workspace built on Qt 6.8.3 (Widgets) with Qt Advanced Docking System 4.4.1
- Disassembly (Zydis/Capstone), control-flow graphs, hex view, pseudocode view
- **Embedded Ghidra 12.1.2 decompiler** — the actual Ghidra decompiler C++ sources compiled into AiDA as a static library, driven by vendored SLEIGH processor specs (x86, x86-64, ARM, AARCH64, MIPS, PowerPC, RISCV, and more)
- Debugger: launch/attach, threads, modules, memory maps, SEH chains, hardware breakpoints via the WhosWho driver
- Memory scanner: AOB/value scans, pointer-scanner, snapshot diffing, crypto constant scanning
- Network toolkit: Burp-style proxy/intercept/repeater/intruder/sequencer/comparer, PCAP capture and export, DNS tooling, TLS keylog support
- **Camoufox browser integration** — the only supported browser backend, chosen for its anti-WebRTC and user-agent privacy guarantees
- AI chat with user-configured providers (OpenAI/Anthropic/Gemini/OpenRouter and others), session persistence in SQLite, cost tracking
- MCP marketplace, scripting, terminal, symbol/kernel tooling, FLIRT-based static library recognition, RTTI/vtable reconstruction, emulation/deobfuscation engines

**IDA Pro plugin**
- Full MCP tool surface over localhost HTTP/SSE: analysis, decompilation, vulnerability analysis, GraphRAG context, mutation tools (patch/rename/type/comment), `execute_python`, multi-IDA instance routing
- Driver-backed emulation and analysis where applicable

**WhosWho driver (functional RE features only)**
- Virtual and physical memory read/write, DTB translation, module/base-address resolution
- Thread context get/set, hardware breakpoints (DR0-DR7)
- Remote calls into target processes
- WFP-based network capture, DNS view, packet injection/redirection, PCAP export
- MalwareSafe: sandboxing of **analysis targets** (a reversing feature, not self-protection)

## Repository Layout

| Path | Contents |
|---|---|
| `src/` | Windows C++ implementation: IDA plugin + shared code + the standalone IDE (`src/standalone/`) |
| `src/standalone/src/qt/` | The Qt 6.8.3 UI (active port; the legacy ImGui/DX11 UI is being retired) |
| `src/standalone/src/core/` | Standalone core: AI/providers, MCP server/tools, RE/disasm/debugger/scanner, network/Burp tooling, sessions, Test Lab |
| `driver/` | WhosWho KMDF driver + user-mode `voyager::device_t` communication API (`driver/comm.h/.cpp`) |
| `mapper/` | WindMapper manual-map loader for WhosWho.sys |
| `sources/ghidra/` | Vendored, modified sparse tree of NSA Ghidra **12.1.2** (`Ghidra_12.1.2_build`) — decompiler sources + SLEIGH processor specs used directly by the build |
| `sources/Triton/` | Vendored Triton source (symbolic execution), consumed by CMake at this exact path |
| `libdecomp/` | Vendored [libdecomp](https://github.com/dmaivel/libdecomp) source (decompiler framework) |
| `.deps/MemPDB/` | Vendored [MemPDB](https://github.com/nikgeneburn/MemPDB) fork with local changes — in-memory kernel PDB symbol engine |
| `cmake/` | CMake modules, including the Qt 6.8.3 dependency gate |
| `tools/` | Build/development utilities |
| `docs/` | Documentation |
| `build-host.cmd` | The supported build wrapper (see below) |

## Building From Source

### Prerequisites

| Requirement | Exact version / notes |
|---|---|
| OS | Windows 10/11 x64 |
| Compiler | MSVC 2022 (v143 toolset), C++17 — one TU (`kernel_symbols.cpp`) compiles as C++20 |
| Build system | **CMake 3.25+** with the **Ninja** generator |
| Qt | **Qt 6.8.3, `msvc2022_64` build — this exact version** (details below) |
| Python | Python 3.12 on PATH (runs `src/encrypt_whoswho.py` for the driver embed step) |
| Assembler | MASM (`ASM_MASM`, ships with MSVC) |
| IDA SDK | Only for the IDA plugin target; not required for the standalone IDE |

### Qt 6.8.3 — Exact Version and Setup

AiDA pins **Qt 6.8.3, MSVC 2022 64-bit (`msvc2022_64`)**. Not 6.8.0, not 6.9.x — 6.8.3.

The CMake gate is `cmake/aida_qt6_dependency.cmake`. It hard-requires:

```
.deps/qt/6.8.3/msvc2022_64/lib/cmake/Qt6/Qt6Config.cmake
```

Required Qt modules: **Core, Gui, Widgets, Network, Svg, Test**.

How to get exactly that:

- **Qt online installer**: select Qt 6.8.3 → MSVC 2022 64-bit, then copy/place the install so the repo sees `.deps/qt/6.8.3/msvc2022_64/`.
- **aqtinstall** (headless):
  ```powershell
  pip install aqtinstall
  aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O .deps/qt
  ```
- You will also need **Qt Advanced Docking System 4.4.1** sources at `.deps/qt-ads-src` (clone `github.com/githubuser0xFFFF/Qt-Advanced-Docking-System`, tag/branch 4.4.1).

If `Qt6Config.cmake` is absent, configure stops with a `FATAL_ERROR` telling you exactly this — that is intentional.

Qt is **dynamically linked** and staged app-locally: after each build, `windeployqt` copies the required Qt runtime next to the executable. AiDA compiles with `QT_NO_KEYWORDS` (the codebase uses `Q_EMIT`/`Q_SIGNALS`/`Q_SLOTS` exclusively).

### Vendored Dependencies (In This Repo)

These are committed directly — you do **not** need to download them:

| Path | What it is |
|---|---|
| `sources/ghidra/` | Ghidra 12.1.2 sparse tree: `Ghidra/Features/Decompiler/src/decompile/cpp` (239 files, built as a static library) + `Ghidra/Processors/*/data/languages` SLEIGH specs incl. prebuilt `x86-64.sla` |
| `sources/Triton/` | Triton symbolic-execution framework (referenced by `add_subdirectory` at this exact path) |
| `libdecomp/` | libdecomp decompiler framework |
| `.deps/MemPDB/` | MemPDB PDB parser (local fork with modifications) — the build `FATAL_ERROR`s without it |
| `src/standalone/resources/` | Fonts, icons, stylesheets, and the embedded Ghidra spec qrc |

### Provisioned Dependencies (.deps)

Large third-party dependencies are **not** all committed (multi-GB). CMake pins their versions and expects them under `.deps/`; provision them per the references in `CMakeLists.txt` and `cmake/aida_qt6_dependency.cmake`. The notable ones:

- Qt 6.8.3 msvc2022_64 (above)
- z3 4.13.4 (`.deps/z3/z3-4.13.4-x64-win`) — `libz3.dll` is staged app-locally post-build
- Qt-ADS 4.4.1 sources (`.deps/qt-ads-src`)
- capstone, zydis 4.1.1, unicorn, LIEF 0.17.6, brotli, nghttp2, llhttp, zlib, zstd, xz, mimalloc, sqlite, lua 5.4, pcre2, sol2, taskflow, parallel-hashmap, concurrentqueue, json-schema-validator, minizip-ng, lmdb, AngelScript, bitwuzla, retdec, remill
- Camoufox browser bundle (see [Running AiDA](#running-aida))

### Build Commands

The only supported preset is `ninja-msvc-release` (see `CMakePresets.json`). Use the wrapper:

```powershell
# Incremental build (normal case)
.\build-host.cmd

# Full clean rebuild including drivers
.\build-host.cmd -FullClean

# Driver rebuild only
.\build-host.cmd -Drivers -CleanDrivers

# Dry-run plan (prints what would happen)
.\build-host.cmd -PlanOnly -FullClean
```

Outputs land in `build-ninja/Release/` (`AiDAStandalone.exe`, the IDA plugin, and driver binaries). The wrapper writes logs under `%TEMP%\aida-build-*` and a machine-readable summary at `%TEMP%\aida_build_summary.json`.

### Kernel Driver (WhosWho + WindMapper)

1. Build `driver/WhosWho/WhosWho.sln` (contains WhosWho + WindMapper) in **x64 Release** — or run `.\build-host.cmd -Drivers`. Output: `build-ninja/Release/WhosWho.sys`.
2. The CMake `encrypt_drivers` target runs `src/encrypt_whoswho.py` and regenerates `src/whoswho_embedded.h` (a plain embedded byte array — **generated file, never hand-edit it**). The driver binary must exist before configure, or `driver_loader.cpp` fails on the missing header.
3. Rebuild the full solution so the updated header links in.
4. **A reboot is required to load an updated kernel driver** — WhosWho has no unload routine. Test-signed/unsigned driver loading is your responsibility on your own analysis machine.

At runtime, `src/driver_loader.cpp` stages the embedded driver and invokes WindMapper first, falling back to a plain `NtLoadDriver`/registry-service path. Driver load failure is non-fatal: RE tools simply gate on `driver_bridge::is_loaded()`.

The user↔kernel ABI structs live in `driver/comm.h` and `driver/WhosWho/WhosWho/src/.../Struct.h` and must stay synchronized (packing, field order, sizes, static asserts). IOCTLs are static: `0x00220000 | ((0x800+offset)<<2)`, device name `\\.\WhosWho`.

### IDA Pro Plugin

Requires IDA Pro with the Hex-Rays decompiler and the IDA SDK (not redistributed here). The plugin exposes its MCP server on localhost; see `src/mcp_server.cpp` and `src/agent_tools.cpp` for the tool surface.

## Running AiDA

- `AiDAStandalone.exe` boots straight into the IDE. No key prompt, no login, no activation, no network requirement.
- **Camoufox is the only supported browser backend.** Discovery checks the repo-local bundle first (`camoufox-135.0.1-beta.24-win.x86_64/camoufox.exe`), then existing build/dependency fallbacks. Browser automation, web search, interception, and reverse-MCP workflows all go through Camoufox for its anti-WebRTC and user-agent privacy guarantees.
- MCP servers bind to `127.0.0.1` only. They expose mutating tools — treat localhost as a trust boundary.
- Logs you will care about: `aida_debug.log` (app/runtime), `%TEMP%\aida_bootstrap.log`, `C:\Users\Public\Desktop\aida_kernel.log` (driver-side), `C:\Users\Public\Desktop\aida_full_test.log` (Test Lab).

## MCP Servers and Tooling

Both the IDA plugin and the standalone IDE run localhost MCP servers (HTTP/SSE) with a shared tool-registry pattern: name, description, JSON schema, `read_only` flag, handler. Mutating tools (patch, rename, type, comment, memory writes, `execute_python`) are marked `read_only=false`. Cross-instance routing uses `instance_id`/`pid` so multiple IDA instances can be addressed individually.

## Testing

- **Test Lab**: in-app functional test suites executed on the background work queue (`src/standalone/src/core/testlab/`). Full-run evidence lands in `C:\Users\Public\Desktop\aida_full_test.log`.
- Qt-side tests use Qt6::Test and run offscreen via CTest (`aida_qt6_add_qtest`).
- Driver-touching tests have real side effects on the host — read them before running.

## Privacy and Security Model

- **No telemetry, no update server, no license server, no phone-home.** The only legitimate outbound traffic is user-configured AI provider endpoints, public package registries (for the MCP marketplace), and Camoufox browsing itself.
- User AI-provider keys are stored DPAPI-obfuscated locally (`core/auth/auth_store.cpp`, `core/settings/standalone_settings.hpp`). This protects the *user's* secrets; it is not self-protection of the app.
- Raw credentials, API keys, OAuth tokens, DPAPI plaintext, TLS keys, and captured traffic bodies are never logged.
- The WhosWho driver contains no self-protection, anti-debug, anti-dump, hiding, attestation, heartbeat, or server contact.

## Contributing

You are welcome here. Some ground rules so contributions land smoothly:

1. **Open an issue before large changes.** The codebase moves fast (the Qt port touches hundreds of files); coordination avoids painful rebases.
2. **Read `AGENTS.md` first.** It documents the build system, the driver rebuild pipeline, directory guide, conventions, and the diagnostics lessons we already learned the hard way.
3. **Conventions that matter:**
   - Windows-first C++ (C++17; `kernel_symbols.cpp` is C++20), CMake 3.25+ / Ninja / MSVC.
   - No code comments/docstrings/TODOs in source — code must be self-explanatory (CMake `COMMENT` strings are fine).
   - No stubs, placeholders, or dead code — implementations must be complete and production-grade.
   - Kernel code: `ExAllocatePool2`, pool tags, `RtlSecureZeroMemory`, SEH, explicit IRQL checks; never touch paged pool under a spin lock.
   - Long-running work goes on the existing `work_queue`; initialize/shutdown pairs stay balanced; preserve cancellation tokens.
   - Do not break the driver bridge, the debugger, or the Win32 message pump. The message-pump invariants in `src/standalone/src/main.cpp` are documented in `AGENTS.md` — honor them.
   - No protection/obfuscation/anti-tamper mechanisms. They were deliberately removed; do not reintroduce them.
   - Never commit secrets, API keys, or user-private material.
4. **Driver changes** follow the driver rebuild pipeline above and always come with a "reboot required" note.
5. **Evidence over speculation** for bug reports: include `aida_debug.log` excerpts, repro steps, and environment details.

## Responsible Use

AiDA is a reverse-engineering toolkit with a kernel driver. Use it only on systems, binaries, and data you are authorized to analyze. Loading kernel drivers and intercepting network traffic carry real risk to the host machine — you are responsible for your own analysis environment. This project is provided as-is, without warranty of any kind.

## Third-Party Notices and Acknowledgements

AiDA builds on the work of others — thank you:

- **Ghidra 12.1.2** (National Security Agency, Apache-2.0) — embedded decompiler + SLEIGH specs; notices in `src/standalone/third_party_notices/Ghidra-LICENSE.txt` and `Ghidra-NOTICE.txt`
- **Qt 6.8.3** (The Qt Company, LGPL-3.0/commercial) — dynamically linked, runtime staged via `windeployqt`
- **Qt Advanced Docking System 4.4.1** (LGPL-2.1)
- **Triton** (Apache-2.0), **libdecomp**, **MemPDB**, **Z3** (MIT), **Capstone**, **Zydis**, **Unicorn**, **LIEF**, **LLVM**, **Taskflow**, **mimalloc**, **PCRE2**, **zstd**, **liblzma**, **nghttp2**, **llhttp**, **brotli**, **SQLite**, **Lua**, **sol2**, **parallel-hashmap**, **concurrentqueue**, **nlohmann json-schema-validator**, **AngelScript**, **Bitwuzla**, **Camoufox**
- Fonts: **Inter** and **JetBrains Mono** (SIL OFL — notices in `src/standalone/third_party_notices/`)
- Dear ImGui (legacy UI, being retired by the Qt port)

Each vendored dependency retains its own LICENSE file in its directory; additional notices live in `src/standalone/third_party_notices/`.

---

*If you find AiDA useful, star the repo, file issues when things break, and send PRs when you fix them. Happy reversing.*
