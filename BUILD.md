## Pinned upstream

- **7-Zip source** lives under [`vendor/7zip`](vendor/7zip) (clone of [ip7z/7zip](https://github.com/ip7z/7zip).
- **Recorded commit:** `839151eaaad24771892afaae6bac690e31e58384`

## Local patches to vendored 7-Zip

- [`vendor/7zip/CPP/7zip/Archive/DllExports.cpp`](vendor/7zip/CPP/7zip/Archive/DllExports.cpp): `g_hInstance` is no longer `static` so it matches the global `extern` used by `CPP/Windows/DLL.cpp` (same pattern as the full `Format7z` link).

## Build (Windows, Visual Studio 2022)

Use the CMake bundled with VS if `cmake` is not on `PATH`:

```text
"C:\Program Files\Microsoft Visual Studio\2022\<Edition>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
```

From the repo root:

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build/Release/ktar.dll`
