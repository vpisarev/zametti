# Cross-compilation for Windows x86-64 via mingw-w64 from a Linux machine.
#
# Same rule as in linux-zsys.cmake: we build NOT against the system we sit on.
# Here that comes for free -- the target world lives in /usr/x86_64-w64-mingw32
# and does not overlap the host /usr/include at all -- but CMAKE_FIND_ROOT_PATH_MODE
# is pinned anyway: without it find_library() happily finds host .so files and
# silently breaks the build at link time, where the message no longer names the
# cause.
#
# The triple is chosen with ZWIN_TRIPLE (x86_64 by default) -- for a possible
# future arm64 build using this same file.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT ZWIN_TRIPLE)
    set(ZWIN_TRIPLE x86_64-w64-mingw32)
endif()

# DRIVERS WITH THE -posix SUFFIX, AND THIS IS NOT A MATTER OF TASTE.
#
# mingw-w64 has two THREAD MODELS, and Debian installs both: "win32" (native
# threads, no pthread at all) and "posix" (threads via winpthreads, linked in
# statically by the -static below). The bare x86_64-w64-mingw32-g++ is win32.
#
# posix is chosen for the sake of THIRD-PARTY TREES. Every vendored library
# that mentions pthread.h at all was checked: libwebp, libsodium, highway, zstd
# and gtest guard it with the correct `#if defined(_WIN32)` and use native
# threads on Windows, but libgav1 (src/utils/threadpool.cc) guards it with
# `#if defined(_MSC_VER)` -- i.e. only MSVC sees the CreateThread/
# WaitForSingleObject branch, while mingw goes down the pthread_create path,
# which does not exist in the win32 model at all. That is a bug in foreign
# code, fixable with one line in two files -- but 3rdparty/libgav1/update.sh
# starts with `rm -rf upstream`, and such a fix would vanish SILENTLY on the
# first update. Fixing it in one place that nobody overwrites is more reliable.
#
# The price, stated honestly: winpthreads inside the binary (tens of kilobytes)
# and the requirement to build Qt with THE SAME drivers. The second is no small
# thing: the thread model decides which libstdc++ gets picked up, and mixing
# objects of two models in one program means two different std::mutex under
# one name.
set(CMAKE_C_COMPILER   ${ZWIN_TRIPLE}-gcc-posix)
set(CMAKE_CXX_COMPILER ${ZWIN_TRIPLE}-g++-posix)
set(CMAKE_RC_COMPILER  ${ZWIN_TRIPLE}-windres)
set(CMAKE_AR           ${ZWIN_TRIPLE}-ar CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB       ${ZWIN_TRIPLE}-ranlib CACHE FILEPATH "" FORCE)

# Search roots: the mingw world and our prefix with what we built (Qt and the rest).
set(CMAKE_FIND_ROOT_PATH /usr/${ZWIN_TRIPLE})
if(DEFINED ENV{ZWPREFIX})
    list(APPEND CMAKE_FIND_ROOT_PATH $ENV{ZWPREFIX})
endif()

# Programs are host ones (cmake, ninja, perl, python, moc via QT_HOST_PATH);
# everything else comes only from the target world.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# SEARCH ONLY FOR STATIC ARCHIVES. In mingw, next to every `libX.a` lies
# `libX.dll.a` -- a tiny import library that contains no code and merely
# promises to find the DLL ON THE TARGET MACHINE. find_library() sees both by
# default and takes the latter; linking passes, the program builds -- and runs
# only where that DLL lies next to it.
#
# We hit exactly this: `-fopenmp` (our resampler and color conversion,
# zametti-core/image) produced `OpenMP_gomp_LIBRARY = libgomp.dll.a`, and
# libgomp-1.dll appeared in the dependency list of zametti.exe -- the only
# non-system DLL out of thirty-eight. On Linux the same did not happen:
# libgomp.so exists on any machine, so the question never came up.
#
# By trimming the suffix list we make such a substitution IMPOSSIBLE wherever
# the library is looked up by find_library().
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")

# ...but OpenMP is looked up DIFFERENTLY, and the suffix alone is not enough.
# FindOpenMP does not call find_library at all: it parses the driver's verbose
# link line and takes the path the driver named for -lgomp -- which is
# libgomp.dll.a. So we name the archive ourselves, by asking the driver
# directly. The path is not hard-coded: the gcc version and the thread model in
# it change, while -print-file-name answers for the very build we compile with.
execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=libgomp.a
                OUTPUT_VARIABLE ZWIN_LIBGOMP OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(ZWIN_LIBGOMP AND EXISTS "${ZWIN_LIBGOMP}")
    set(OpenMP_gomp_LIBRARY "${ZWIN_LIBGOMP}" CACHE FILEPATH "static libgomp" FORCE)
endif()

# The program must be ONE file with no foreign stuff next to it -- the same
# goal as the portable Linux build. mingw has its own three things for that,
# and all three are needed: without -static, libgcc_s_seh-1.dll,
# libstdc++-6.dll and libwinpthread-1.dll would land next to it, and "but it
# works on my machine" here only means the DLLs were found in the build
# directory.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")

# Windows headers: request the modern API (Win10) and suppress the min/max
# macros that break std::min/std::max in third-party trees. Via *_FLAGS_INIT
# rather than add_compile_definitions(): the toolchain is read inside
# try_compile too, while directory-level commands do not reach there -- the
# checks of Qt and of third-party CMake files would see a different set of
# definitions than the build itself.
#
# WIN32_LEAN_AND_MEAN and UNICODE are deliberately NOT set globally: the former
# strips pieces out of windows.h that third-party trees rely on, the latter
# changes the meaning of a whole family of functions for libraries written
# against the char API. Qt declares both for itself.
set(CMAKE_C_FLAGS_INIT   "-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNOMINMAX")
set(CMAKE_CXX_FLAGS_INIT "-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNOMINMAX")

# WHAT TO RUN THE BUILT BINARIES WITH. Cross-building usually means "the test
# suites are not run": ctest tries to execute an .exe on Linux and gets Exec
# format error. Wine removes that boundary, and CMake can plug it in by itself
# -- then `ctest` in the build directory works just like on a native machine.
#
# This is a SMOKE check, not acceptance: "green under wine" and "green under
# Windows" are different claims, and the second is verified only on a real
# machine. But the first catches exactly the class of trouble we are porting
# for: a non-portable path, a file opened in text mode, a missing platform
# plugin.
find_program(ZWIN_WINE wine)
if(ZWIN_WINE)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${ZWIN_WINE}")
else()
    message(STATUS "wine not found -- .exe files are built, but ctest will not be able to run them")
endif()
