# scum_simple_rcon_ue4ss

Minimal UE4SS C++ Mod that exposes a Source RCON listener and dispatches raw SCUM admin command text on the game thread.

This intentionally implements only the scope requested:

- UE4SS C++ mod lifecycle: `start_mod`, `uninstall_mod`, `on_unreal_init`.
- Source RCON server: TCP listener, password auth, request/response packet framing, multi-packet replies.
- Game-thread queue: RCON worker threads enqueue commands; EngineTick drains and executes them.
- Native AdminCommand execution: `!exec <SteamID64> <command> [arguments]` resolves one exact online player, uses that player's real network-backed `ConZPlayerController` and live `Prisoner`, then invokes SCUM's native `AdminCommand` executor. It never creates a synthetic controller, pawn, interface or connection.
- Unified RCON endpoint: normal admin commands and notifications enter through the authenticated, game-thread queue. `SendChat` has a deliberate chat-only branch: it uses SCUM `MiscStatics:SendChatLineToPlayer` against real, online target controllers, never a fabricated or arbitrary `PlayerRpcChannel`.
- Console-command identity guard: unprefixed fallback commands require `SCUM/Saved/Config/WindowsServer/AdminUsers.ini` to contain a valid SteamID64 unless disabled in config. Native `!exec` uses its explicit online execution context.

The implementation fails closed when a required SCUM binding, native executor
signature, or reflected function schema does not match the expected runtime
shape. It reports the mismatch instead of guessing an address, object, or
network channel.

## Runtime behavior

1. UE4SS loads `ue4ss/Mods/scum_simple_rcon/dlls/main.dll`.
2. The mod reads `ue4ss/Mods/scum_simple_rcon/config.ini`.
3. If the password is still `CHANGE_ME_BEFORE_USE`, the listener refuses to start.
4. After RCON auth, any command except `rcon.status`, `rcon.admins`, and `rcon.help` is queued to the game thread. `rcon.dispatch` and `rcon.chat` are safe game-thread diagnostics.
5. For `!exec`, the game thread resolves the exact SteamID to a live network-backed controller and verifies its `Prisoner` and `AdminCommandExecutor`. The command is constructed with that real controller as Outer.
6. The mod verifies the current SCUM image fingerprint and build prologue without scanning the multi-megabyte `.text` section. It derives the shared response method and its adjacent executor slot from live `AdminCommand` CDO vtables, requires one unique consensus across the registry, and then calls the selected command's own executor. A mismatch or native exception fails closed. Unprefixed Unreal console commands use the server-console fallback.

`rcon.status` reports whether the game-thread consumer is actually alive, so a
broken drain is distinguishable from a failing command. A healthy result is
`ok; game-thread queue active; drains=<n>` with a rising counter. If the
EngineTick hook is missing, or is installed but has never fired, it returns
`DEGRADED` with the reason instead of failing every later command as a timeout.

Native AdminCommands require an explicitly selected online execution context because SCUM's executor dereferences the real player controller and pawn. `SendChat` does not need `AdminUsers.ini`, but it also requires each recipient to be online with a real network-backed controller. A player-targeted notification needs its recipient online to be delivered or displayed. The selected native context does not imply that the command affects only that player.

## Example commands

```text
rcon.status
rcon.admins
rcon.dispatch
rcon.chat
!exec 7656119XXXXXXXXXX CheckServerTime
!exec 7656119XXXXXXXXXX SpawnItem Water_01 1
Announce Hello from RCON
SendChat 4 "Bounty claimed: +500" 7656119XXXXXXXXXX
SendChat 2 "xxx killed xxx"
SendNotification 2 0 "Welcome!" 7656119XXXXXXXXXX
```

For `!exec`, the supplied SteamID is required and must match exactly one
connected controller. It selects SCUM's execution context and is not appended to
the native command arguments. For `SendChat`, an omitted SteamID broadcasts by
resolving and sending once to each real online controller. A supplied SteamID
routes only to that controller. Chat requires the controller's inherited `Player`
object to be a real `UNetConnection`.

The last example is a broadcast only to real online players. Do not add an
inline `# ...` comment to an RCON command: it is command text, not a shell
script, so it would become part of the message/argument stream.

Run `rcon.chat` after world load to validate the non-admin chat route without
sending anything. A healthy result contains `real chat dispatcher ready` and
reports the number of real online controllers; zero is valid on an empty server.

Do not prefix commands with `#`; if you do, this mod strips one leading `#` before dispatch.

## AdminUsers.ini

Default lookup:

```text
<ServerInstall>\SCUM\Saved\Config\WindowsServer\AdminUsers.ini
```

The parser accepts any line containing a 17-digit SteamID64, so these all work:

```ini
7656119XXXXXXXXXX
Admin=7656119XXXXXXXXXX
+AdminUsers=7656119XXXXXXXXXX
```

No SteamID is compiled into the DLL and no online `PlayerRpcChannel` is used as
an admin surrogate. The file and optional `[admin] preferred_steam_id` guard only
the unprefixed server-console fallback. `!exec` instead requires its exact online
context in the command itself.

## Native-dispatch diagnostics

Run `rcon.dispatch` after the world has loaded. A healthy result contains:

```text
ok: native AdminCommand dispatcher ready; native_dispatch=enabled; native_executor=enabled
```

`executor=...` must report a matched SCUM build fingerprint, the build function,
the live response function and derived response/executor vtable slots, the
observed response consensus, and how many registered commands match that
contract. Resolution uses the PE header, one pinned build prologue, and bounded
CDO vtable reads rather than a `.text` scan. `game_thread_elapsed_us` separates
plugin dispatch time from the rest of the RCON round trip. A requested verb
outside the matching set is rejected before object construction. If SCUM changes
its executable code or command layout, the checks fail before invocation.
Update and retest on a non-production server against the actual SCUM and UE4SS
build.
