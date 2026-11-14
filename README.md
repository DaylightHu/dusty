# mcmux
mcmux is an experimental redstone simulator written in C, based on Minecraft 1.16.2. It started as a project to learn raylib and later evolved into an effort to combine redstone construction with programming.

The goal is to give users control over circuit inputs, execution backends, and simulation time, enabling faster execution of circuits built entirely from redstone. The computation remains in the circuits themselves, with their logic and in-game timing preserved.


> [!TIP]
> **📖 API Reference**
>
> Explore public functions, grouped by purpose, with concise descriptions.
>
> [**English →**](https://daylighthu.github.io/mcmux/mcmux-api-en.html) · [**简体中文 →**](https://daylighthu.github.io/mcmux/mcmux-api-zh.html)
>

## Build

Use a C compiler to compile `nob.c` into the `nob` build runner. Then run the
`./nob` commands from the project root. On Windows, use the equivalent
`nob.exe` form.

| Command | Purpose |
| --- | --- |
| `./nob` or `./nob build` | Build the project. |
| `./nob gui` | Build the GUI. |
| `./nob run` | Build and launch the GUI. |
| `./nob reader` | Build the standalone `.mcs` structure reader. |
| `./nob benchmark` | Generate C queries, compile them, and run the full redstone benchmark suite. |
| `./nob test-c` | Generate, compile and test the C query backend. |
| `./nob export-c INPUT.mcs OUTPUT.c [SYMBOL]` | Export an MCS circuit as a C query model. |
| `./nob vendor-list` | List the vendored raylib files required by the build. |
| `./nob precommit` | Stage the required raylib files and verify an index-only build. |
| `./nob clean` | Remove build outputs. |
| `./nob help` | Show the available build commands. |

## Use mcmux

| Command | Purpose |
| --- | --- |
| `Ctrl+b` then `%` | Open a terminal or split left/right. |
| `Ctrl+b` then `"` | Split top/bottom. |
| `Ctrl+b` then `g` | Restore and focus mc-gui. |
| `Ctrl+b` then `x` | Close the focused pane. |
| `Ctrl+b` then `,` / `r` | Open settings in Neovim / reload settings. |
| `Ctrl+b` then `?` | Show workspace bindings. |

### Terminal panes

A terminal pane runs your shell from the project root. On Linux and macOS it
uses a PTY and the bundled libvterm. Run `nvim` in the pane to use your own
Neovim configuration. Use `Ctrl+Shift+V` to paste and `Shift+PageUp` or
`Shift+PageDown` to scroll. On Windows the world GUI is available and terminal
panes are unsupported. Press the workspace leader twice to pass it into the
focused terminal.

Set `[terminal].font_size` and optionally `[terminal].font` in
`config.toml`. The font setting accepts a path to a monospace TTF.

### C drivers

Use `./nob build-c FILE` to build and `./nob export-c INPUT.mcs OUTPUT.c SYMBOL`
to export a model. To run a driver from an embedded shell, execute
`"$MCMUX_HOST" --run-driver "$(realpath FILE)"`. mcmux saves its open worlds,
releases the world sessions and sends build and driver output to the calling
terminal. Reopen the save in mc-gui to see the driver's changes.
Use `Ctrl+b` then `k` to cancel the driver.

For optional Neovim commands, source
[`integrations/nvim/mcmux.lua`](integrations/nvim/mcmux.lua) from your own
`init.lua`. It registers commands inside mcmux terminal panes and needs
Neovim 0.11 or later. Available commands are `:McBuild`, `:McRun` and
`:McModel`. Build and run save the current C file first and show output in a
Neovim terminal buffer.

## mc-gui

World and simulation commands are entered in the GUI with `/` or `//`.

When the crosshair targets redstone dust, the top-left HUD shows its current
power level (0–15), including while ticking is frozen. The power line is hidden
when targeting any other block or empty space.

| Command | Purpose |
| --- | --- |
| `//pos1 X Y Z` / `//pos2 X Y Z` | Set the inclusive selection corners. |
| `//copy [-a] [X Y Z]` | Copy the selection with an optional anchor; `-a` skips air on subsequent pastes of this clipboard. |
| `//paste [-a] X Y Z` | Paste at an anchor; `-a` skips air. |
| `//stack [-a] [COUNT] DIRECTION` | Add repeated copies in a direction; `-a` skips air. |
| `//move [-a] [DISTANCE] DIRECTION` | Move the selected blocks; `-a` moves only non-air blocks. |
| `//undo` / `//redo` | Undo or redo the last selection edit. |
| `//schem save [-f] NAME [X Y Z]` | Save the selection beside the current world's `.mcw`; `-f` replaces an existing file. |
| `//schem load NAME` | Load an `.mcs` beside the current world's `.mcw` into the clipboard, preserving its anchor and container contents. Use `//paste X Y Z` to place it. |
| `/tick freeze` | Toggle automatic ticking. |
| `/tick step NUMBER` | Run a fixed number of ticks while frozen. |
| `/tick noupdate` | Toggle block callbacks and neighbor updates. |
| `/plot info` | Show the current plot bounds. |
| `/plot expand DIRECTION [COUNT]` | Expand the plot. |
| `/plot shrink DIRECTION [COUNT]` | Shrink the plot; confirm if edits would be deleted. |

## World directories

Each world is stored as a self-contained directory under `saves/`. The
directory and its `.mcw` file use the same world name. Programs and structures
belonging to that world can be stored beside the world file.

The repository includes an example world at `saves/examples/`.

## References

- https://github.com/raysan5/raylib
- https://github.com/tsoding/nob.h
- https://github.com/jdah/minecraft-weekend
- https://github.com/MCHPR/MCHPRS
- https://github.com/gnembon/fabric-carpet
- https://github.com/maruohon/tweakeroo
- https://github.com/EngineHub/WorldEdit
- https://github.com/tmux/tmux
- https://github.com/neovim/neovim
- https://github.com/cktan/tomlc17
