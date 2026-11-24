# Voxilog

Voxilog is a redstone circuit workbench written in C and based on Minecraft
Java Edition 1.16.2. Build and inspect circuits in a raylib 3D view, then use C
drivers to set inputs, advance the simulation by game ticks, and check outputs.

## Command reference

| Command | What it does |
| --- | --- |
| `voxi list` | Print saved world names. |
| `voxi create NAME` | Create and save a world from the terminal. |
| `voxi load NAME` | Open an existing world in the 3D view. |
| `voxi delete NAME` | Permanently delete a world and its directory. |
| `voxi run-c DRIVER.c` | Compile and run a C driver. |
| `voxi export-c DRIVER.c` | Generate the C model declared in a driver. |
| `voxi export-c INPUT.mcs OUTPUT.c [SYMBOL]` | Generate a C model from a saved circuit structure. |
| `voxi --help` | Show command usage. `voxi -h` and `voxi` also show help. |

### World names and saves

Choose a new `NAME` when creating a world. To load or delete a world, use a
name from `voxi list`, which prints one name per line. A world named
`timing-lab` is stored at `saves/timing-lab/timing-lab.mcw`. Names are 1–63
ASCII characters and may contain letters, digits, hyphens, underscores, and
interior spaces. Quote a name that contains spaces, as in
`voxi create "Timing Lab"`.

`voxi create NAME` rejects an existing world name. `voxi delete NAME` also
removes any structures and other files stored inside `saves/NAME/`.

Existing scripts can use `voxi view NAME` as an alias for `voxi load NAME`.
The `voxi world` prefix is still accepted for the world commands.

### Run a C driver

```sh
voxi run-c saves/examples/not_gate.c
```

`DRIVER.c` is a C source file inside the repository. Voxilog compiles it
against `build/libmcmux.a` and runs it from the project root. The compiled
driver is stored under `build/programs/`, which mirrors the source directory
layout. Set the `CC` environment variable to choose a compiler. The default
is `cc` on Linux and macOS and `clang` on Windows.

The NOT gate example checks the output at each tick and writes the resulting
state back to the bundled example files. For a driver that includes a generated
model, export the model before running the driver.

### Export a C model

An exported model lets a driver evaluate redstone logic using generated C
while the simulator manages world state and game ticks.

When a driver contains a model declaration, pass just the driver path.

```sh
voxi export-c saves/examples/carry_cancel_array.c
voxi run-c saves/examples/carry_cancel_array.c
```

The first command creates `build/carry_cancel_array_model.c`. The second
compiles the driver with that model and runs the 16-bit adder example. The
driver supplies the input path, output path, and model name in a single
line comment. Paths are relative to the working directory.

```c
// mcmux-model: saves/examples/carry_cancel_array.mcs build/carry_cancel_array_model.c carry_cancel_array_model
```

For a standalone structure, supply the same arguments directly.

```sh
voxi export-c saves/examples/carry_cancel_array.mcs build/carry_cancel_array_model.c carry_cancel_array_model
```

| Argument | Meaning |
| --- | --- |
| `INPUT.mcs` | Path to a saved Voxilog circuit structure. |
| `OUTPUT.c` | Destination for the generated C source. Its parent directory must exist. |
| `SYMBOL` | C identifier the driver uses to refer to the generated model. Use up to 128 ASCII letters, digits, or underscores and start with a letter. This argument is optional in the direct form and defaults to `mcmux_circuit`. |

## Build from source

Run these commands from the repository root on Linux or macOS.

```sh
cc -std=c11 -O2 nob.c -o nob
./nob build
```

The Linux build requires X11 and OpenGL development libraries. On Windows,
build with a Clang toolchain and make `llvm-windres` or `windres` available.

```powershell
clang -std=c11 -O2 nob.c -o nob.exe
.\nob.exe build
```

Add the directory containing the built `voxi` command to your shell's `PATH`
to use the examples above.

## C API reference

- [English](https://daylighthu.github.io/voxilog/mcmux-api-en.html)
- [简体中文](https://daylighthu.github.io/voxilog/mcmux-api-zh.html)

## References

- [raylib](https://github.com/raysan5/raylib)
- [MCHPRS](https://github.com/MCHPR/MCHPRS)
- [Carpet](https://github.com/gnembon/fabric-carpet)
- [Tweakeroo](https://github.com/maruohon/tweakeroo)
- [WorldEdit](https://github.com/EngineHub/WorldEdit)
- [Verilog](https://www.verilog.com)
