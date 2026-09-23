# tf2perf

Thx HugeGpt.4.1

Performance hooks for Team Fortress 2. It injects a DLL into the game, hooks client-side
functions that cost a lot on weak CPUs, and exposes a CLI so you can turn each one on or off
while playing.

sponsored by winrar.world

## Hooks

| Hook | Effect |
|---|---|
| CViewRender::RenderView | frame counter, budget resets, caches the view origin |
| TF_3rdPersonMuzzleFlashCallback (+ sentry) | caps muzzle flash sprites per frame |
| CParticleMgr per-effect simulate | per-frame particle simulation budget |
| C_BaseAnimating::UpdateClientSideAnimations | stops updating far away animations |
| CClientShadowMgr depth pass + PreRender | shadow on/off |
| CEngineVGui::Simulate | VGUI panel logic throttle |
| C_BaseEntity::DrawBrushModel, C_LocalTempEntity::DrawStudioModel | distance culling |
| C_BaseAnimating::SetupBones | skips no-output calls past a distance (experimental) |
| cl_particle_retire_cost | turns on the cheat-flagged particle budget |

## VAC

it a dll can get you vac

## Build

Needs Visual Studio 2022 or newer with the x64 C++ tools, and MinHook built as a static
x64 lib.

1. get MinHook from github.com/TsudaKageyu/minhook and build libMinHook.x64.lib
2. if it is not in C:\mh, set MINHOOK to the folder
3. run build.cmd

Output: tf2perf.dll, tf2perf.exe, sigcheck.exe.

sigcheck.exe checks all hook patterns against your client.dll and engine.dll and should
print 14/14 passed. Hooks whose patterns stop matching are skipped at runtime instead of
patching the wrong code.

## Usage

Run tf2perf.exe. Double-clicking works.

On start it:
1. looks for the TF2 process with client.dll loaded
2. injects tf2perf.dll from the same folder as the exe, if it is not in there yet
3. connects and waits for commands

If TF2 is not running it says so and waits. Start the game, then type `inject`.

`menuoff` is on by default, so every setting is suspended outside a match and menus behave
stock. Type `help` for the command list, `quit` to exit.

```
tf2perf             attach or inject, stay open
tf2perf inject      inject, then stay open
tf2perf status      print hooks and settings once and exit
```

CLI commands:

```
retire 1            particle screen area budget, 0 = off, try 0.5-2
muzzleflash 4       max muzzle flash sprites per frame
simbudget 48        max particle effects simulated per frame
animdist 2000       stop updating animations past 2000 units
shadows off         skip the shadow passes
far 3000            skip brush models and temp ents past 3000 units
vguisim 2           VGUI panel logic every 2nd frame
status              hooks, counters, current settings
alloff              turn every setting off
help                full command list
```

Log: %TEMP%\tf2perf.log

## Recommended settings

Type these in the CLI after `tf2perf inject`:

```
retire 1
muzzleflash 4
simbudget 48
animdist 2000
shadows off
vguisim 2
far 3000
status
```

`animdist` only skips animations past 2000 units, so nearby players and taunts stay smooth.
`alloff` turns everything off at once.

## License

MIT, see LICENSE.
