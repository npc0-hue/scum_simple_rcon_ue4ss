# Install

1. Build the mod; see `BUILD.md`.
2. Copy the built folder into the SCUM server Win64 directory:

```text
<ServerInstall>\SCUM\Binaries\Win64\ue4ss\Mods\scum_simple_rcon\
```

Expected layout:

```text
Win64/
  dwmapi.dll
  SCUMServer.exe
  ue4ss/
    UE4SS.dll
    UE4SS-settings.ini
    Mods/
      mods.txt
      scum_simple_rcon/
        enabled.txt
        config.ini
        dlls/
          main.dll
```

3. Edit `config.ini`:

```ini
[rcon]
bind_address = 127.0.0.1
port = 27015
password = use-a-real-password
```

4. Enable the mod in `ue4ss/Mods/mods.txt`:

```text
scum_simple_rcon : 1
```

5. Reduce `ue4ss/UE4SS-settings.ini` to the hooks this mod needs.

UE4SS enables roughly sixteen hooks by default. On SCUM 1.3.3 those extra hooks
take the game down with an `EXCEPTION_ACCESS_VIOLATION` the moment a player joins
(`Match State Changed from WaitingToStart to InProgress`, then a fatal error
about 0.2 s later). Keep only the two hooks the mod depends on:

```ini
[Hooks]
HookProcessInternal = 0
HookProcessLocalScriptFunction = 0
HookInitGameState = 0
HookLoadMap = 0
HookCallFunctionByNameWithArguments = 1
HookBeginPlay  = 0
HookEndPlay  = 0
HookLocalPlayerExec = 0
HookAActorTick = 0
HookEngineTick = 1
HookGameViewportClientTick = 0
HookUObjectProcessEvent = 0
HookProcessConsoleExec = 0
HookUStructLink = 0
```

`HookEngineTick` drives the game-thread command queue. `HookCallFunctionByNameWithArguments`
captures the original function pointer the mod falls back to when
`ProcessConsoleExec` refuses a command.

6. Start the server and check `ue4ss/UE4SS.log` for:

```text
[scum_simple_rcon] rcon: listening on 127.0.0.1:27015
```

7. Test locally:

```bat
mcrcon -H 127.0.0.1 -P 27015 -p use-a-real-password rcon.status
mcrcon -H 127.0.0.1 -P 27015 -p use-a-real-password rcon.admins
mcrcon -H 127.0.0.1 -P 27015 -p use-a-real-password rcon.dispatch
mcrcon -H 127.0.0.1 -P 27015 -p use-a-real-password rcon.chat
```

Security note: keep `bind_address = 127.0.0.1` unless you are putting this behind a firewall/VPN. Source RCON is not encrypted.

Before sending a native `AdminCommand`, connect a test player and use that
player's exact 17-digit SteamID64 as the execution context:

```bat
mcrcon -H 127.0.0.1 -P 27015 -p use-a-real-password "!exec 7656119XXXXXXXXXX CheckServerTime"
```

`!exec` requires a live network-backed `ConZPlayerController`, a live `Prisoner`
pawn, and the real `AdminCommandExecutor` interface. It never creates a dummy
player. The first SteamID selects the execution context and is not appended to
the native arguments.

Ordinary `SendChat` is separate: it only needs an authenticated RCON client and
a real online recipient. Unprefixed fallback commands still use the optional
`AdminUsers.ini` guard and intentionally do not fall back to the first online
player.

Wait until the SCUM world has loaded, then run `rcon.status`, `rcon.admins`, and
`rcon.dispatch`. A healthy native probe reports the verified SCUM build
fingerprint, the build function, the live response function, the derived
response/executor vtable slots, and how many registered commands match that
contract. A verb outside the matching set is rejected before object
construction. `game_thread_elapsed_us` reports time spent waiting for and
running the game-thread probe. Before production use, run one harmless native
command with an exact connected context and confirm both a successful response
and a healthy SCUMServer process. If any identity, interface, class, Outer or
vtable check fails, do not force an address or retry blindly.
