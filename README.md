### BotStory fork

This is the BotStory fork of [iw2d/kaentake](https://github.com/iw2d/kaentake).
On top of the upstream hooks (resolution, quickslot, tooltip, etc.) it adds:

- **Self-patching launcher.** `Launch Game.exe` (the renamed `Kaentake.exe`)
  now runs a patcher before spawning `MapleStory.exe`. On launch it fetches
  `manifest.txt` from a configured patch URL, SHA-256s local files (with a
  `.patch_cache` side-file so unchanged files skip rehashing), and pulls any
  files that differ. Atomic `.new` → rename; two-retry on hash mismatch.
- **Bootstrap distribution.** Friends get a ~40 KB zip with only the
  launcher + config + onboarding docs; the ~6.6 GB of game files download
  on first launch over Tailscale. See `../patch-server/README.md` for the
  end-to-end flow.
- **Version guard.** `v83_version_guard.cpp` bails at DLL attach if the
  host `MapleStory.exe` isn't the v83 build the offsets were written for.

The patcher code lives in `src/patcher.{h,cpp}` and is wired into
`src/launcher.cpp` ahead of `DetourCreateProcessWithDllExA`. It links
`winhttp` + `bcrypt` (both shipped with Windows; no external deps).

---

### Usage

For end users: extract the bootstrap zip, run `Launch Game.exe` as
administrator. The patcher handles the rest.

For upstream-style usage: download all files from the original
[Releases](https://github.com/iw2d/kaentake/releases) into a clean v83
installation directory, then run `Kaentake.exe` as administrator.

A standalone WZ file (`Custom.wz`) is included to extend the system option
dialog with a combobox for selecting the in-game resolution.

---

### Configuration

Either the commandline arguments or the `config.ini` file can be used to
redirect the client to a host other than the default `"127.0.0.1"`.

Commandline (takes precedence over the file):
```
Launch Game.exe 127.0.0.1 8484
```

`config.ini`:
```
[config]
host=127.0.0.1
port=8484
patch_url=http://<server-ip>:8485
resolution=1600x900
```

- `host` / `port` — Cosmic login server address.
- `patch_url` — optional. If unset, the patcher is skipped entirely and the
  launcher behaves like stock upstream. If set, the patcher fetches
  `<patch_url>/manifest.txt` on every launch.
- `base` — optional, the WZ set the client is built on. Leave it out (or
  `repack`) for the Community Repack set. `base=cosmic` marks a client built
  on the clean Cosmic WZ set (patched with the server repo's
  `tools/wz-patcher`): the patcher also fetches
  `<patch_url>/bases/cosmic/manifest.txt`, whose entries replace the main
  manifest's WZ files and download from `<patch_url>/bases/cosmic/`. If the
  server has no such manifest the launcher asks before playing with the
  current files; it never falls back to the Repack WZ files.
- `resolution` — optional preferred launch resolution. Accepts either a
  `WxH` shorthand (`800x600`, `1024x768`, `1366x768`, `1600x900`,
  `1920x1080`) or a direct index (`0`–`4`). When set, it overrides the
  user's saved in-game preference for every launch — useful for shipping
  a sensible default in the bootstrap zip. The quickslot bar variant
  (8-slot / wider) is picked to match. **In-game changes sync back**:
  if this line is present, changes made via System Option → Resolution
  are mirrored to `config.ini` on clean exit, so the file always reflects
  the latest choice. Leave the line out entirely to hand resolution
  control exclusively to the in-game menu.