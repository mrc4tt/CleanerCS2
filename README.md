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

## Gamedata

The plugin hooks tier0's internal `LogDirect`, which is not exported, so it needs a
signature. It reads `addons/cleanercs2/gamedata.json` and falls back to the signature
compiled into the plugin when the file is missing or its signature no longer matches.
After a game update, replacing `gamedata.json` is enough; no rebuild is needed.

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

## Building from source

[Xmake](https://xmake.io/#/getting_started) is used to build the project.

1. Clone the repository and its submodules
2. Run xmake in the root directory of the repository