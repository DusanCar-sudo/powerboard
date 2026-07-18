# Build Instructions

## Linux (Debian/Ubuntu)

```bash
# Install dependencies
sudo apt install -y cmake g++ libsqlite3-dev git

# Clone and build
git clone https://github.com/milodule3-debug/Aura-FLUX.git
cd Aura-FLUX
mkdir build && cd build
cmake ..
cmake --build . -j

# Install
sudo apt install ./aura-flux_0.2.3_amd64.deb
aura-flux
```

## Linux (source install)

```bash
sudo cmake --install build
```

## Windows (Preview)

**Requirements**: Visual Studio 2019+, vcpkg, git

```cmd
# Install vcpkg
git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat

# Install ftxui and sqlite3
.\vcpkg install ftxui:x64-windows sqlite3:x64-windows

# Clone and build
git clone https://github.com/milodule3-debug/Aura-FLUX.git
cd Aura-FLUX
mkdir build
cd build

# Configure with vcpkg toolchain
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build . --config Release

# Run
.\Release\powerboard.exe
```

**Note**: Windows build is preview-only (metrics return dummy values). Full Windows port planned.

## macOS (Preview)

**Requirements**: Xcode, Homebrew, git

```bash
# Install dependencies
brew install cmake sqlite3

# Clone and build
git clone https://github.com/milodule3-debug/Aura-FLUX.git
cd Aura-FLUX
mkdir build && cd build

# Install ftxui via vcpkg
git clone https://github.com/Microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg
~/vcpkg/install ftxui sqlite3

cmake .. -DCMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . -j

# Run
./powerboard
```

**Note**: macOS build is preview-only (metrics return dummy values). Full macOS port planned.

## Troubleshooting

**Linux**: Missing power profiles daemon? Optimisation tab features unavailable (safe fallback).

**Windows/macOS**: Clipboard vault & AI features require native APIs (stubbed in preview).

**Full cross-platform support**: See roadmap in README.
