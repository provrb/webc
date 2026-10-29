# webc

A web crawler in C++23. Dependencies (Boost.Beast/Asio, SQLite) are listed in
`vcpkg.json` and installed automatically by CMake on first configure.

## Setup (once per machine)

You need CMake 4+, Ninja, clang, and a clone of [vcpkg](https://github.com/microsoft/vcpkg)
with `VCPKG_ROOT` pointing at it.

### macOS

```sh
xcode-select --install
brew install cmake ninja pkg-config
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
echo 'export VCPKG_ROOT="$HOME/vcpkg"' >> ~/.zshrc && source ~/.zshrc
```

The project uses `std::print` and deducing `this`, so it needs a recent Xcode.
If Apple clang is too old, `brew install llvm` and configure with
`-DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++"`.

### Windows

Install LLVM, CMake, Ninja and Visual Studio (for the MSVC headers/libs), clone and
bootstrap vcpkg (`bootstrap-vcpkg.bat`), then:

```sh
setx VCPKG_ROOT C:\vcpkg
```

## Build & run

```sh
cmake --preset dev
cmake --build --preset dev
./build/dev/main
```

The first configure builds the dependencies, which takes a few minutes.

`build/dev/compile_commands.json` is what clangd uses (see `.clangd`). Re-run
`cmake --preset dev` after adding a source file, since sources are globbed.
