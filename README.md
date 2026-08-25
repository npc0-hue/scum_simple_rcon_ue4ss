# scum_simple_rcon_ue4ss

Minimal UE4SS C++ Mod that exposes a Source RCON listener and dispatches raw SCUM admin command text on the game thread.

This intentionally implements only the scope requested:

- UE4SS C++ mod lifecycle: `start_mod`, `uninstall_mod`, `on_unreal_init`.
- Source RCON server: TCP listener, password auth, request/response packet framing, multi-packet replies.
- Game-thread queue: RCON worker threads enqueue commands; EngineTick drains and executes them.
- Offline AdminCommand execution: builds a synthetic `BP_ConZPlayerController` + `ConZCharacter` in the dedicated-server world, then invokes SCUM's native `AdminCommand` executor. An administrator does **not** need to be online.
- Unified RCON endpoint: normal admin commands and notifications enter through the authenticated, game-thread queue. `SendChat` has a deliberate chat-only branch: it uses SCUM `MiscStatics:SendChatLineToPlayer` against real, online target controllers, never a fabricated or arbitrary `PlayerRpcChannel`.
- Admin identity guard: reads `SCUM/Saved/Config/WindowsServer/AdminUsers.ini` and refuses game commands when no configured admin SteamID64 exists, unless disabled in config.

The implementation fails closed when a required SCUM binding, native executor
signature, or reflected function schema does not match the expected runtime
shape. It reports the mismatch instead of guessing an address, object, or
network channel.

## Runtime behavior

1. UE4SS loads `ue4ss/Mods/scum_simple_rcon/dlls/main.dll`.
2. The mod reads `ue4ss/Mods/scum_simple_rcon/config.ini`.
3. If the password is still `CHANGE_ME_BEFORE_USE`, the listener refuses to start.
4. After RCON auth, any command except `rcon.status`, `rcon.admins`, and `rcon.help` is queued to the game thread. `rcon.dispatch` and `rcon.chat` are safe game-thread diagnostics.
5. The game thread checks `AdminUsers.ini`, discovers SCUM `AdminCommand` classes, and creates/reuses a synthetic server-only caller.
6. The mod resolves SCUM's native AdminCommand executor with one exact signature. It only calls the executor when exactly one match is found; otherwise it returns a clear error. Non-AdminCommand Unreal console commands use the server-console fallback.

Normal server admin commands can therefore run while the player count is **zero**. `SendChat` is not an admin command in this mod: it does not need `AdminUsers.ini`, but it does require each recipient to be online with a real network-backed controller. A player-targeted notification likewise needs its recipient online in order to be delivered/displayed; neither feature requires the administrator to be online.

## Example commands

```text
rcon.status
rcon.admins
rcon.dispatch
rcon.chat
Announce Hello from RCON
SendChat 4 "Bounty claimed: +500" 7656119XXXXXXXXXX
SendChat 2 "xxx killed xxx"
SendNotification 2 0 "Welcome!" 7656119XXXXXXXXXX
```

For `SendChat`, an omitted SteamID broadcasts by resolving and sending once to
each real online controller. A supplied SteamID routes only to that controller.
The implementation requires the controller's inherited `Player` object to be
a real `UNetConnection`, so synthetic offline admin controllers and
class-default objects are deliberately excluded.

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
an admin surrogate. The file is re-read after changes; with multiple configured
admins the dispatcher selects the lexicographically first valid ID, unless the
optional `[admin] preferred_steam_id` names one that is also present in the file.
An already-created synthetic caller refreshes its selected identity after a file
change.

## Offline-dispatch diagnostics

Run `rcon.dispatch` after the world has loaded. A healthy result contains:

```text
synthetic=ready; executor=native AdminCommand executor resolved by exact signature
```

If SCUM changes its executable code, the signature may no longer match. In that case the mod deliberately refuses to call a guessed address; update the signature for that SCUM build and retest on a non-production server.
