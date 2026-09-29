# Build

UE4SS C++ mods need to be built with the same UE4SS source/ABI line as the runtime you deploy with.

This project compiles against UE4SS's **public headers only**. Every UE4SS symbol it uses is imported from `UE4SS.dll` at load time, so the UE4SS project itself — including its Rust dependencies — is never built. You only need a RE-UE4SS checkout for its `UE4SS/include` and `deps/first/*/include` trees.

## Folder layout

Either keep a sibling checkout:

```text
MyMods/
  RE-UE4SS/
  scum_simple_rcon_ue4ss/
```

or point the build at a checkout anywhere on disk:

```sh
cmake -B build -DUE4SS_SOURCE_DIR=/path/to/RE-UE4SS
```

## Configure and build

Visual Studio:

```bat
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Game__Shipping__Win64 --target scum_simple_rcon
```

Ninja:

```bat
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Game__Shipping__Win64
cmake --build build --target scum_simple_rcon
```

Output package:

```text
build/package/ue4ss/Mods/scum_simple_rcon/
  config.ini
  enabled.txt
  dlls/main.dll
```

The header closure also reads `deps/first/Unreal`, which RE-UE4SS tracks as a
submodule. If your checkout is missing it, initialize it first:

```bat
git submodule update --init --depth 1 deps/first/Unreal
```

## Import library

UE4SS marks its API `__declspec(dllimport)` for mods, so the link needs an
import library. Building all of UE4SS to obtain one would drag in its Rust
toolchain, so `ue4ss-def/UE4SS.def` lists the exports of the `UE4SS.dll` this
mod is deployed against and `lib.exe` turns that list into the import library:

```bat
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Game__Shipping__Win64 ^
  -DUE4SS_SOURCE_DIR=C:\path\to\RE-UE4SS ^
  -DUE4SS_DEF_FILE=ue4ss-def\UE4SS.def
```

Regenerate `ue4ss-def/UE4SS.def` whenever the target server's `UE4SS.dll`
changes, so the exported symbol names keep matching:

```sh
python3 ue4ss-def/generate_def.py UE4SS.dll ue4ss-def/UE4SS.def
```

## Continuous integration

`.github/workflows/build-windows.yml` builds the DLL on `windows-2022` and
uploads it as an artifact.

It needs a repository secret `UE4SS_PAT`, set under
**Settings -> Secrets and variables -> Actions**. Use a classic personal access
token with the `repo` scope: the job checks out `npc0-hue/RE-UE4SS`, whose
`deps/first/Unreal` submodule lives in the private `Re-UE4SS/UEPseudo`, and a
user-scoped token reaches both while a repository-scoped deploy key cannot.
The workflow rewrites the SSH submodule URLs to authenticated HTTPS so the
submodule fetch uses the same token.

Required UE4SS hooks/settings:

```ini
[Hooks]
HookEngineTick = 1
HookUObjectProcessEvent = 1
```

`HookEngineTick` is mandatory because RCON worker threads must never create
UObjects or call the SCUM command executor themselves. The mod performs that
work only from the game-thread drain callback.
