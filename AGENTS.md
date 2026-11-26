# Development guide

This repository is a UE4SS C++ mod for a SCUM dedicated server. Its purpose is
to provide a password-protected Source RCON endpoint while keeping all Unreal
object work on the game's thread.

## Runtime architecture

```text
RCON TCP worker
  -> CommandQueue
  -> EngineTick game-thread drain
  -> ScumBridge
       -> RealChatDispatcher       (ordinary SendChat)
       -> NativeAdminExecutor      (!exec <SteamID64> AdminCommand path)
       -> server console fallback  (non-AdminCommand console commands)
```

- `RconServer` owns sockets, Source-RCON framing, authentication, and response
  splitting. It must never create Unreal objects or call Unreal APIs.
- `CommandQueue` is the sole boundary between RCON threads and the game thread.
  Keep all `UObject` lookup, `ProcessEvent`, object creation, and command
  execution inside its EngineTick drain.
- `SimpleRconMod` exposes the internal diagnostics: `rcon.status`,
  `rcon.admins`, `rcon.dispatch`, and `rcon.chat`.
- `ScumBridge` routes `SendChat` and `!exec` before checking `AdminUsers.ini`.
  Unprefixed game commands still use the server-console fallback and retain the
  configured-admin guard.

## Admin-command route

- Native dispatch uses `!exec <SteamID64> <AdminCommand> [arguments]`. The ID
  must match exactly one live network-backed `ConZPlayerController`, and that
  controller must have a live `Prisoner` pawn and implement
  `AdminCommandExecutor`.
- Construct the registered command class with that real controller as its Outer.
  Validate the class, vtable slot and Outer before the native call. Never create a
  synthetic controller, pawn, interface pointer or network connection.
- Resolve the native executor only through its exact, build-specific signature.
  Derive its vtable slot from live command CDOs. On zero or multiple matches or
  inconsistent slots, fail closed and do not call a guessed address.
- A native exception leaves execution status unknown, latches native dispatch
  off, and forbids automatic retry until the SCUM process restarts.
- Unprefixed server-console commands still read valid administrator SteamID64
  values from `SCUM/Saved/Config/WindowsServer/AdminUsers.ini`. Never hard-code
  one and never substitute an arbitrary connected player or `PlayerRpcChannel`.

## Ordinary-chat route

`SendChat <type 0-7> "message" [SteamID64]` is deliberately not an
`AdminCommand`.

- It requires RCON authentication, but not `AdminUsers.ini`.
- Resolve SCUM's real server-side entry point at runtime:
  `MiscStatics:SendChatLineToPlayer`, its `Default__MiscStatics`, and
  `ConZPlayerController:GetUserId`.
- A targeted send must match the supplied SteamID64 against an actual loaded
  `ConZPlayerController` whose inherited `Player` is a live `UNetConnection`.
  A missing/offline target is an error.
- A broadcast is a separate send to each such real online controller. `type`
  controls the SCUM chat style; recipient scope comes from the optional
  SteamID64, not from `type`.
- Do not construct a chat controller, fabricate a network/RPC channel, use the
  first `PlayerRpcChannel`, or impersonate a player. The SCUM helper receives
  the real target controller and uses the game's own delivery path.
- Inspect the UFunction parameter schema through reflection before calling it.
  If SCUM changes the schema, reject the command and log the discovered schema
  instead of guessing parameter offsets.

## Change and test rules

- Keep command parsing and messages UTF-8-safe. Quoted RCON arguments contain
  spaces; do not append shell-style inline comments to command examples.
- Keep `SendChat` semantics separate from notifications and native admin
  commands. A chat recipient and a native execution context must both be online;
  the native context is selected explicitly and must not be the first online
  player by accident.
- Update `README.md` and `INSTALL.md` with every externally visible behavior
  change. Describe what this repository does; do not retain stale feature
  comparison or proprietary-bundle claims.
- Before committing, run `git diff --check`. On a Windows server built against
  the deployed UE4SS ABI, test `rcon.status`, `rcon.admins`, `rcon.dispatch`,
  `rcon.chat`, one targeted `SendChat`, and one harmless `!exec` command with an
  exact connected player. Require a native success and a healthy server process;
  never use a dummy caller.
- macOS-only checks cannot validate the final DLL/SCUM ABI. Do not claim a
  Windows runtime test passed until it has run on the target server.
