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

5. Start the server and check `ue4ss/UE4SS.log` for:

```text
[scum_simple_rcon] rcon: listening on 127.0.0.1:27015
```

6. Test locally:

```bat
mcrcon -H 127.0.0.1 -P 27015 -p use-a-real-password rcon.status
mcrcon -H 127.0.0.1 -P 27015 -p use-a-real-password rcon.admins
mcrcon -H 127.0.0.1 -P 27015 -p use-a-real-password rcon.dispatch
mcrcon -H 127.0.0.1 -P 27015 -p use-a-real-password rcon.chat
```

Security note: keep `bind_address = 127.0.0.1` unless you are putting this behind a firewall/VPN. Source RCON is not encrypted.

Before sending an `AdminCommand` (for example `SetTime`, `SpawnItem`, or an
admin-targeted `SendNotification`), make sure the server has:

```text
<ServerInstall>\SCUM\Saved\Config\WindowsServer\AdminUsers.ini
```

with at least one 17-digit SteamID64. This mod intentionally does not fall back to the first online player.

Ordinary `SendChat` is different: it only needs an authenticated RCON client
and a real online recipient. It does not need `AdminUsers.ini`, does not create
a controller/channel, and refuses a targeted send when that SteamID is not
currently connected.

For the no-player test, wait until the SCUM world has loaded, leave the server
empty, then run `rcon.dispatch` followed by a harmless command such as:

```bat
mcrcon -H 127.0.0.1 -P 27015 -p use-a-real-password SetTime 12
```

`rcon.dispatch` must report `synthetic=ready`. If the native executor signature
is not found, do not force an address: the SCUM build changed and the mod needs
an updated, tested signature.
