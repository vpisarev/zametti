# Toolchain the Qt and OpenSSL in $ZPREFIX were built with. Passed in by
# packaging/linux/qtbase-configure.sh; no need to set it by hand.
# Requires the environment from packaging/linux/zenv.sh (ZSYS, ZPREFIX).
#
# DEBT, NAMED OUT LOUD: this file and linux-zsys.cmake (which builds the
# program itself) are almost identical; the one difference is that the latter
# pins the six XCB_*_LIBRARY to the static archives in $ZPREFIX. Two
# almost-identical toolchains are divergence waiting to happen; merging them
# into one is possible, but that changes what the already-installed Qt was
# built with and so requires rebuilding and re-verifying it.
#
# CMAKE_SYSTEM_NAME IS DELIBERATELY NOT SET. Same architecture, the build stays
# "native", and Qt does not demand QT_HOST_PATH with a separate host Qt.
# Setting it would declare a cross build and set up a second Qt build for
# nothing.

set(CMAKE_SYSROOT "$ENV{ZSYS}")
set(CMAKE_C_COMPILER "$ENV{ZSYS}/bin/gcc")
set(CMAKE_CXX_COMPILER "$ENV{ZSYS}/bin/g++")

# THE KEY PART. Without this find_library() happily takes the HOST's
# /usr/lib/x86_64-linux-gnu (Ubuntu 26.04, glibc 2.43), the build passes, and
# portability leaks silently, exactly as it leaked through absolute symlinks
# inside the sysroot.
set(CMAKE_FIND_ROOT_PATH "$ENV{ZSYS}" "$ENV{ZPREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Programs (perl, python, ninja, wayland-scanner), however, must be the HOST's:
# they run here and now, not on the target machine.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# Mandatory: libstdc++ of gcc-15 requires GLIBCXX_3.4.32, which is absent even
# in Ubuntu 20.04. Without this the result will not run on the target system.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")

# The static libraries end up inside a PIE program.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
