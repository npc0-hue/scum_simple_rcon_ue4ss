# Build

UE4SS C++ mods need to be built with the same UE4SS source/ABI line as the runtime you deploy with. Build this beside a working RE-UE4SS checkout.

## Folder layout

```text
MyMods/
  CMakeLists.txt
  RE-UE4SS/
  scum_simple_rcon_ue4ss/
```

`MyMods/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.22)
project(MyMods)

add_subdirectory(RE-UE4SS)
add_subdirectory(scum_simple_rcon_ue4ss)
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

If your UE4SS source checkout cannot initialize `deps/first/Unreal`, link your GitHub account to Epic Games and rerun:

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
