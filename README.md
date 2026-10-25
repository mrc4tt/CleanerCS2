# CleanerCS2

A simple Metamod plugin that allows you to filter out console prints with regular expressions.
Supports both Windows and Linux.

## Example config

```
// Write regular expression in here to prevent them from being printed in the console
.*UNEXPECTED LONG FRAME DETECTED.*
.*weapon services didn't find a shoot position.*
.*generating substitute command \d+ from \d+.*
```

## Installation

1. Download the latest release from the [releases page](https://github.com/Source2ZE/CleanerCS2/releases/)
2. Extract the contents of the archive to your server's `addons` directory
3. Edit the `addons/cleanercs2/config.cfg` file to your liking (see the example config above)

## Channel and block rules

Besides regular expressions, the config understands a few special lines:

| Line | Effect |
|------|--------|
| `@channel:Name` | Drops every message on the logging channel `Name` (e.g. `SteamNetSockets`, `Server`, `EngineServiceManager`). `General` is the channel used by `Msg`/`Warning`. |
| `@channel[2]:Name` | Drops only messages with severity <= 2 (1 = detailed, 2 = message, 3 = warning, 4 = assert, 5 = error). |
| `@block:regex` | Drops the message matching `regex` and the `{ ... }` dump that follows it on the same channel, e.g. the KeyValues dump after `CSource2Server::ApplyGameSettings`. |
| `@stderr:regex` | Linux only. Drops lines written directly to stderr that bypass the logging system, e.g. the Steam API's `dlopen failed trying to load` and `[S_API FAIL]`. Matched per line, without the trailing newline. |

Run `conclear_debug 1` to prefix every message with `[cc2:channel|severity]`, which shows what to filter. Channels that are not registered yet when the config loads are resolved on the next map load.

When at least one `@stderr` rule is present, stderr is redirected through a pipe and filtered line by line; without such rules stderr is left untouched.

## Gamedata

Signatures live in `addons/cleanercs2/gamedata.json`, so they can be updated after a CS2 update without rebuilding the plugin. If the file or an entry is missing, or its signature no longer matches, the built-in signature is used.

```json
{
  "LogDirect": {
    "signatures": {
      "library": "tier0",
      "windows": "4C 89 4C 24 ? 44 89 44 24 ? 89 54 24 ?",
      "linux": "55 89 D0 49 89 FA"
    }
  }
}
```

## Commands

- `conclear_reload` - Reloads the configuration file
- `conclear_debug <0|1>` - Tags every console message with its channel and severity

## Support

Need help, found a bug, or have a feature idea? Join the Discord support server:

[![Discord](https://img.shields.io/badge/Discord-Join%20the%20server-5865F2?logo=discord&logoColor=white)](https://discord.gg/zuQN3U2ZEh)

- **Questions and setup help:** ask in the support channels on Discord.
- **Bug reports:** open a [GitHub issue](https://github.com/mrc4tt/CleanerCS2/issues) with your server log, plugin version and steps to reproduce. Discord is fine for a quick check first.
- **Feature requests:** post them on Discord or open an [issue](https://github.com/mrc4tt/CleanerCS2/issues).

Please do not send support requests by direct message. Asking in the public channels means others can help and find the answer later.

## Building from source

[Xmake](https://xmake.io/#/getting_started) is used to build the project.

1. Clone the repository and its submodules
2. Run xmake in the root directory of the repository