# Voxilog

Voxilog is a C workbench for Minecraft Java 1.16.2 redstone. Build circuits in
3D and drive them from C.

## Commands

Run from the repository root.

| Command | What it does |
| --- | --- |
| `voxi list` | List saved worlds. |
| `voxi create NAME` | Create `saves/NAME/NAME.mcw`. |
| `voxi load NAME` | Open a world in 3D. Try `voxi load examples`. |
| `voxi delete NAME` | Permanently delete `saves/NAME/` and its contents. |
| `voxi run-c DRIVER.c` | Compile and run a C source file inside the repository. |
| `voxi export-c DRIVER.c` | Export using the paths and model name in the driver's `// mcmux-model:` line. |
| `voxi export-c INPUT.mcs OUTPUT.c [SYMBOL]` | Export the structure at `INPUT.mcs` to `OUTPUT.c`. The model name `SYMBOL` defaults to `mcmux_circuit`. |
| `voxi --help` | Show command usage. |

Quote world names with spaces, as in `voxi create "Timing Lab"`.

## World editing

Select two corners with the wooden axe (left and right click), or press `/`
to open the command line and enter `//pos1 X Y Z` and `//pos2 X Y Z`.

| In-game command | Action |
| --- | --- |
| `//replace BLOCK_A BLOCK_B` | Replace matching blocks in the selection. For example, `//replace white_concrete gray_concrete`. |
| `//copy [-a] [X Y Z]` | Copy the selection with an optional anchor. |
| `//paste [-a] [X Y Z]` | Paste at the given position or at the player. |
| `//stack [-a] [N] [DIR]` | Repeat the selection `N` times along `DIR`. |
| `//move [-a] [N] [DIR]` | Move the selection `N` blocks along `DIR`. |
| `//undo`, `//redo` | Step backward or forward through edit history. |
| `//schem save [-f] NAME [X Y Z]`, `//schem load NAME` | Save a selection to `.mcs` or load it into the clipboard. `-f` allows overwrite. |

`-a` skips air. Stack and move default to one step in the look direction.

## Example driver

In voxilog, you write your own drivers. You express the behavior you want through the redstone circuit’s own logic, then use a simulation backend to run the circuit faster.

```sh
voxi export-c saves/examples/carry_cancel_array.c
voxi run-c saves/examples/carry_cancel_array.c
```

This generates `build/carry_cancel_array_model.c` and runs the 16-bit adder.

## Build

On Linux or macOS, build from the repository root. Linux requires X11 and
OpenGL development libraries.

```sh
cc -std=c11 -O2 nob.c -o nob
./nob build
```

On Windows, use Clang with `llvm-windres` or `windres`.

```powershell
clang -std=c11 -O2 nob.c -o nob.exe
.\nob.exe build
```

Add the directory containing `voxi` to your `PATH`.

## Local Minecraft assets

To use local Minecraft textures and fonts, set the absolute path to a 1.16.2 JAR or resource-pack ZIP in `~/.config/voxilog/config.toml` (`%APPDATA%/voxilog/config.toml` on Windows).

```toml
[assets]
resource_pack = '/path/to/1.16.2.jar'
```

## Placement hotkeys

Hold `G` to offset placement or `H` to rotate, aim at a sector of the block
face, and right-click. Rebind them in the same config file.

```toml
[keys.world]
placement_offset = 'G'
placement_rotation = 'H'
```

## C API reference

[English](https://daylighthu.github.io/voxilog/api-en.html) ·
[简体中文](https://daylighthu.github.io/voxilog/api-zh.html)

## References

- [raylib](https://github.com/raysan5/raylib)
- [MCHPRS](https://github.com/MCHPR/MCHPRS)
- [Carpet](https://github.com/gnembon/fabric-carpet)
- [Tweakeroo](https://github.com/maruohon/tweakeroo)
- [WorldEdit](https://github.com/EngineHub/WorldEdit)
- [Verilog](https://www.verilog.com)
- [nob.h](https://github.com/tsoding/nob.h)
