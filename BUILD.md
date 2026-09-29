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
git submodule update --init --recursive
```

Required UE4SS hooks/settings:

```ini
[Hooks]
HookEngineTick = 1
HookUObjectProcessEvent = 1
```

`HookEngineTick` is mandatory because RCON worker threads must never create
UObjects or call the SCUM command executor themselves. The mod performs that
work only from the game-thread drain callback.
