# dcmesh

`dcmesh` is a PC-side offline converter that turns binary glTF files (`.glb`) into `.dcmesh` files for Dreamcast rendering. It uses `cgltf` for loading GLB files and selected `meshoptimizer` sources to reorder and stripify triangle data.

## Requirements

- `gcc`
- `g++`
- `make`
- `meshoptimizer` checked out locally

Clone `meshoptimizer` next to this project, or pass its path with `MESHOPT_DIR`:

```sh
git clone https://github.com/zeux/meshoptimizer
```

## Build

```sh
make MESHOPT_DIR=./meshoptimizer
```

Build output is a single executable in the project root:

```text
dcmesh.exe
```

On non-Windows toolchains the executable is `dcmesh`.

Object files are temporary build intermediates and are removed automatically after linking.

## Usage

```sh
./dcmesh input.glb output.dcmesh
```

If the output path is omitted, the converter writes next to the input file using the `.dcmesh` extension:

```sh
./dcmesh input.glb
```

## Clean

```sh
make clean
```

This removes the executable and any temporary object files.
