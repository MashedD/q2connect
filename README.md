# q2connect

Quake II server browser inspired by Q2PRO's built-in server list. Built with C++23, CMake, raylib, and nlohmann/json like `q2aimtrainer`.

## Build

```sh
./build-lin.sh
```

## Linux Desktop Entry

Use `q2connect.desktop.example` as a template. Replace `/ABSOLUTE/PATH/TO/q2connect` with `/home/user/Projects/q2connect`, then copy it to `~/.local/share/applications/q2connect.desktop`.

## Controls

- `R` or `F5`: refresh all servers
- `Enter` or double click: launch Q2PRO and connect to selected server
- Arrow keys / mouse wheel: move selection
- `1`-`5`: sort by hostname, mod, map, players, RTT
- `C`: copy `connect <address>`
- `Esc`: quit

## Configuration

`hide_wallfly` defaults to `true`. While enabled, players named exactly `WallFly[BZZZ]` are hidden from the player details and excluded from each server's player count and the total player count. Set it to `false` in `q2connect.json` to show and count them.
