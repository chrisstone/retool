# Building retool

This document provides step-by-step instructions for building the `retool` utility on Windows.

## Prerequisites

To build `retool`, you need the following tools installed on your system:
1. **Visual Studio 2022** (Community, Professional, or Enterprise) or **Build Tools for Visual Studio 2022**.
   - Make sure the **Desktop development with C++** workload is selected during installation.
2. **CMake 3.25** or later.
   - If you installed Visual Studio 2022 with C++ support, CMake is already installed as part of the IDE under the Visual Studio directory.

---

## Build Configurations

We support two primary build configurations:
- **Debug**: Standard compilation with full debug symbols (`/Zi`) and optimization disabled (`/Od`). Ideal for testing and debugging.
- **Release**: Fully optimized compilation (`/O2`) with debug symbols stripped or minimal. Ideal for production runs.

---

## Building with CMake Presets (Recommended)

`retool` includes a `CMakePresets.json` file configuring directories and MSVC toolchains automatically.

### 1. Open a Command Prompt or PowerShell
Navigate to the root directory of the `retool` repository:
```powershell
cd path\to\retool
```

### 2. Configure the Project
Run the configure step pointing to your chosen preset configuration:

- **Debug Configuration**:
  ```powershell
  cmake --preset debug
  ```
- **Release Configuration**:
  ```powershell
  cmake --preset release
  ```

### 3. Build the Target
To compile the source code after configuring, build the generated solution:

- **Debug Build**:
  ```powershell
  cmake --build build/debug --config Debug
  ```
  *Output executable path:* `build/debug/Debug/retool.exe`

- **Release Build**:
  ```powershell
  cmake --build build/release --config Release
  ```
  *Output executable path:* `build/release/Release/retool.exe`

---

## Building without Global CMake (Visual Studio Toolchain)

If `cmake` is not added to your global Windows system PATH, you can run the CMake executable included directly inside Visual Studio or Visual Studio Build Tools.

For Visual Studio Build Tools 2022, the typical path is:
`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`

You can configure and compile the build using the full path:

### Configure (Debug)
```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --preset debug
```

### Build (Debug)
```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build/debug --config Debug
```
