# tf2perf — client-side performance hooks for Team Fortress 2

**tf2perf** is a small, source-available performance mod for TF2 (x64 client). It injects a
DLL into the game and hooks client-side hot paths that Valve's own config can't reach, then
exposes them through a command-line interface attached over a named pipe.

**Not malware.** Full source is in this repo, there is no network code, and it does not
persist anything beyond its own log and (optional) config files. Read
[Is this a virus?](#is-this-a-virus) below.

---

## What it hooks

Every target is located by **byte-pattern scanning** at load time and **verified before
patching**. If a signature doesn't match (e.g. after a TF2 update), that hook is skipped and
logged — nothing is ever patched blind.

| Hook | What it does |
|---|---|
| `CViewRender::RenderView` | frame boundary, per-frame budget resets, caches view origin |
| `TF_3rdPersonMuzzleFlashCallback` + `_SentryGun` | caps muzzle-flash sprites per frame |
| `CParticleMgr` per-effect simulate | per-frame particle simulation budget |
| `C_BaseAnimating::UpdateClientSideAnimations` | distance-gated animation updates (`animdist`) |
| `CClientShadowMgr::ComputeShadowDepthTextures` + `PreRender` | shadow pass kill switch |
| `CEngineVGui::Simulate` (engine.dll) | VGUI panel simulation throttle |
| `C_BaseEntity::DrawBrushModel` + `C_LocalTempEntity::DrawStudioModel` | distance culling |
| `C_BaseAnimating::SetupBones` | skips no-output calls beyond a distance (experimental) |
| `cl_particle_retire_cost` | enables the cheat-flagged particle retire budget |

All hook addresses, the vtable slots used (`GetRenderOrigin` = slot 9, `GetPlayerViewSetup`
= slot 12), the `CViewSetup` origin offset, and the animation-list globals were recovered
from the shipping binaries and are validated at runtime where possible.

## Is this a virus?

No. It is an **injectable hook DLL**, and antivirus engines flag that *category* of software
heuristically — the same reason Cheat Engine, OBS game hooks, and every mod injector get
flagged. Concretely, this code:

- **does not** connect to the network (no sockets, no HTTP, no telemetry)
- **does not** write anywhere except `%TEMP%\tf2perf.log` (and its own config file, if you use one)
- **does not** inject into anything except the TF2 process you point it at
- **does not** install persistence, services, or startup entries by itself
- is **entirely** in this repo — `tf2perf.c`, `tf2perf_cli.c`, `sigcheck.c`, `build.cmd`

The only Windows APIs it uses: `CreateRemoteThread`/`LoadLibraryA` (injection), MinHook
(detours), `CreateNamedPipe` (IPC between the DLL and the CLI), `fopen` (log). You can read
all of it, and you can build it yourself.

### ⚠️ VAC warning

This hooks the game process. On VAC-secured servers, **that may be detected and result in a
ban**. Use it on your own local servers, on community servers where you accept the risk, or
not at all. It is not affiliated with or endorsed by Valve.

## Build

Requirements: Visual Studio 2022+ (or Build Tools) with the x64 C++ toolset, and
[MinHook](https://github.com/TsudaKageyu/minhook) built as a static x64 lib.

1. Build MinHook (or grab a prebuilt `libMinHook.x64.lib` + `MinHook.h`).
2. Point `build.cmd` at it — set `MINHOOK` (defaults to `C:\mh`):
   ```
   set MINHOOK=C:\path\to\minhook
   build.cmd
   ```
3. Output: `tf2perf.dll`, `tf2perf.exe`, `sigcheck.exe`.

### Verify before you run

```
sigcheck.exe
```
Maps `client.dll` and `engine.dll` and checks all hook patterns against the expected
addresses. You should see `14/14 passed`. If TF2 updates and this drops below 14, the
affected hooks will be skipped at runtime instead of patching the wrong code.

## Usage

```
tf2perf inject      find TF2 (the process with client.dll loaded), inject, drop into the CLI
tf2perf             attach to an already-injected game (REPL)
tf2perf status      one-shot: print hooks, addresses, counters, settings
```

Then, in the CLI:

```
retire 1              particle screen-area budget (0 = off; try 0.5-2)
muzzleflash 4         max 3rd-person muzzle flashes per frame
simbudget 48          max particle effects simulated per frame (freezes far trails)
animdist 2000         skip animation updates beyond 2000 units (near players stay smooth)
shadows off           kill depth shadows + shadow manager PreRender
far 3000              cull brush models + temp-ent models beyond 3000 units
vguisim 2             VGUI panel simulation every 2nd frame
status                show hooks + counters
alloff                disable every setting at once
help                  full command list
```

`%TEMP%\tf2perf.log` records what resolved and what didn't at injection time.

## How it works (short version)

The DLL waits for `client.dll`, pattern-scans the targets listed above, validates each one,
and installs MinHook detours. Settings live in globals that the detours read; the CLI talks
to the DLL over `\\.\pipe\tf2perf` so toggles apply live without a restart. Anything that
doesn't resolve is skipped and logged, so a TF2 update degrades to "fewer optimizations",
not "crash".

## License

MIT — see `LICENSE`.
