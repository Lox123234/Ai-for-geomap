# vcpkg triplet for GeoMap AI on Windows: 64-bit DLLs, release only, built with the same
# MSVC toolset as the application (v143, Visual Studio 2022). The deployment ships the VC++
# runtime of that toolset next to the EXE; libraries built with a newer toolset could need a
# newer runtime than the one shipped.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_PLATFORM_TOOLSET v143)
