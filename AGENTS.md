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
       -> OfflineAdminExecutor     (AdminCommand path)
       -> server console fallback  (non-AdminCommand console commands)
```

- `RconServer` owns sockets, Source-RCON framing, authentication, and response
  splitting. It must never create Unreal objects or call Unreal APIs.
- `CommandQueue` is the sole boundary between RCON threads and the game thread.
  Keep all `UObject` lookup, `ProcessEvent`, object creation, and command
  execution inside its EngineTick drain.
- `SimpleRconMod` exposes the internal diagnostics: `rcon.status`,
  `rcon.admins`, `rcon.dispatch`, and `rcon.chat`.
- `ScumBridge` routes `SendChat` before checking `AdminUsers.ini`; all other
  game commands follow the admin/offline-dispatch route.

## Admin-command route

- `OfflineAdminExecutor` may create and reuse the synthetic
  `BP_ConZPlayerController` and `ConZCharacter`. This exists only so native
  SCUM `AdminCommand` execution can run with no human administrator online.
- Read valid administrator SteamID64 values from
  `SCUM/Saved/Config/WindowsServer/AdminUsers.ini`. Never hard-code one and
  never substitute an arbitrary connected player or `PlayerRpcChannel`.
- The selected ID must come from that file (the optional preferred ID is valid
  only when also present there). Refresh a cached synthetic caller if the file
  changes.
- Resolve the native executor only through its exact, build-specific signature.
  On zero or multiple matches, fail closed and report the problem; do not call a
  guessed address or weaken the game's authorization check.

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
- Keep `SendChat` semantics separate from notifications and admin commands.
  An administrator may be offline; a chat recipient cannot be offline.
- Update `README.md` and `INSTALL.md` with every externally visible behavior
  change. Describe what this repository does; do not retain stale feature
  comparison or proprietary-bundle claims.
- Before committing, run `git diff --check`. On a Windows server built against
  the deployed UE4SS ABI, test `rcon.status`, `rcon.admins`, `rcon.dispatch`,
  `rcon.chat`, one targeted `SendChat`, and one broadcast `SendChat`.
- macOS-only checks cannot validate the final DLL/SCUM ABI. Do not claim a
  Windows runtime test passed until it has run on the target server.
