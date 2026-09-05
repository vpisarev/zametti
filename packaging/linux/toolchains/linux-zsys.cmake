# Toolchain for the portable Linux build: everything is built against the
# Ubuntu 20.04 sysroot (glibc 2.31), not against the system we are sitting on.
#
#   cmake -S . -B build-portable -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=packaging/linux/toolchains/linux-zsys.cmake \
#         -DWITH_STATIC_QT=ON
#   cmake --build build-portable -j8
#
# NOTHING ELSE IS NEEDED: no environment, no zenv.sh. That one is needed only
# when building THE DEPENDENCIES THEMSELVES (Qt, OpenSSL, xcb helpers); see
# docs/zametti-build-linux.md.
#
# The Qt and OpenSSL in ${ZDEPS_ROOT} were built with this same file. By
# design: diverging switches between Qt and the program give a binary that
# builds but does not run, and that is the most expensive thing to debug
# afterwards.

# WHERE THE SYSROOT AND THE BUILT DEPENDENCIES LIVE. Three sources, strongest
# first: -DZSYS_ROOT= on the command line, environment variables (ZSYS/ZPREFIX),
# the usual place in ~/work. THE ANSWER GOES INTO THE CACHE, and that is not a
# convenience: `make` re-runs cmake by itself on every CMakeLists edit, with no
# environment left, and the build used to fail out of nowhere with "ZSYS not
# set" although the build directory was configured and working.
if(NOT DEFINED ZSYS_ROOT)
    if(DEFINED ENV{ZSYS})
        set(ZSYS_ROOT "$ENV{ZSYS}")
    else()
        set(ZSYS_ROOT "$ENV{HOME}/work/zsys")
    endif()
endif()
if(NOT DEFINED ZDEPS_ROOT)
    if(DEFINED ENV{ZPREFIX})
        set(ZDEPS_ROOT "$ENV{ZPREFIX}")
    else()
        set(ZDEPS_ROOT "$ENV{HOME}/work/zdeps")
    endif()
endif()
set(ZSYS_ROOT "${ZSYS_ROOT}" CACHE PATH "Ubuntu 20.04 sysroot everything is built against")
set(ZDEPS_ROOT "${ZDEPS_ROOT}" CACHE PATH "where the built Qt, OpenSSL and xcb helpers are installed")
# Trial builds (try_compile) create their own cache and re-read this file;
# without this line they would take the default rather than what was
# configured here.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ZSYS_ROOT ZDEPS_ROOT)

if(NOT IS_DIRECTORY "${ZSYS_ROOT}/usr/include")
    message(FATAL_ERROR
        "ZSYS_ROOT=${ZSYS_ROOT} does not look like a sysroot: no usr/include.\n"
        "  Build one per docs/zametti-build-linux.md or point to your own:\n"
        "  cmake -DZSYS_ROOT=/path/to/sysroot ...")
endif()
if(NOT EXISTS "${ZSYS_ROOT}/bin/gcc")
    message(FATAL_ERROR
        "no ${ZSYS_ROOT}/bin/gcc — compiler wrappers are not installed.\n"
        "  bash packaging/linux/fix-sysroot.sh")
endif()

# Qt and OpenSSL are looked up RIGHT HERE: -DCMAKE_PREFIX_PATH on the command
# line is no longer needed. One given from outside stays: a user's own path
# always beats ours.
list(APPEND CMAKE_PREFIX_PATH "${ZDEPS_ROOT}")

# PKG-CONFIG LOOKS ONLY IN THE SYSROOT, and that is set HERE, not in the
# environment. Qt finds xkbcommon-x11 and half of X11 through pkg-config;
# without these two lines it asked the system we sit on, and either did not
# find them (configure failed with "Qt6::XcbQpaPrivate not found") or found
# FOREIGN ones, and portability leaked silently. LIBDIR, not PATH: PATH only
# adds paths to the system ones, and we need the system ones cut off.
# usr/share/pkgconfig is mandatory: that is where wayland-protocols.pc lives.
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${ZSYS_ROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
    "${ZSYS_ROOT}/usr/lib/x86_64-linux-gnu/pkgconfig:${ZSYS_ROOT}/usr/share/pkgconfig")
unset(ENV{PKG_CONFIG_PATH})

# CMAKE_SYSTEM_NAME IS DELIBERATELY NOT SET. Same architecture, the build stays
# "native", and Qt does not demand QT_HOST_PATH with a separate host Qt.
# Declaring a cross build would mean setting up a second Qt build for nothing.

set(CMAKE_SYSROOT "${ZSYS_ROOT}")
set(CMAKE_C_COMPILER "${ZSYS_ROOT}/bin/gcc")
set(CMAKE_CXX_COMPILER "${ZSYS_ROOT}/bin/g++")

# THE KEY PART. Without this find_library() happily takes the HOST's
# /usr/lib/x86_64-linux-gnu, the build passes, and portability leaks silently,
# exactly as it leaked through absolute symlinks inside the sysroot itself.
set(CMAKE_FIND_ROOT_PATH "${ZSYS_ROOT}" "${ZDEPS_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Programs (perl, python, ninja, wayland-scanner, moc), however, must be the
# HOST's: they run here and now, not on the target machine.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# MANDATORY, not optional: libstdc++ of gcc-15 requires GLIBCXX_3.4.32, which
# is absent even in the Ubuntu 20.04 the sysroot was taken from. Without these
# switches the result does not run on the target system (measured).
# -s strips symbols at link time (owner's decision, 2026-09-04): the portable
# binary is for users, not for a debugger, and the symbol table of a static
# build with Qt inside weighs megabytes. Executables only: the portable build
# has no shared libraries.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc -s")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")

# THE xcb-util FAMILY AND SM/ICE: STATIC.
#
# The six libraries pulled in by Qt's xcb plugin (xcb-cursor, xcb-icccm,
# xcb-image, xcb-keysyms, xcb-render-util, xcb-util) live in packages with
# priority=EXTRA, the lowest: they are never part of a base install and appear
# on a machine only if something else pulled them in. libxcb-cursor0 is the one
# that makes Qt 6.5+ fail with "could not load the Qt platform plugin xcb";
# it is the most common complaint about deploying Qt programs.
#
# All six are in the sysroot as archives, and they are thin wrappers over
# libxcb, so linking them in is cheaper than explaining to a person which six
# packages to install.
#
# libSM and libICE (X11 session) are DELIBERATELY NOT HERE, although their
# archives lie right next door and it was tempting. Static libICE.a requires
# arc4random_buf from libbsd, and libbsd is not shipped in the sysroot as an
# archive at all; linking ICE in would trade two priority=optional libraries
# for one of the same kind plus hassle. Both stay dynamic: every desktop with
# X11 has them.
#
# MUST BE SET HERE, NOT ONLY WHEN BUILDING Qt. The FindXCB.cmake module is
# installed TOGETHER with Qt and looks the libraries up afresh for every
# consumer, so it is our build's cache that decides, not the cache of the Qt
# build. The first attempt set them only for Qt, and exactly one of
# thirty-seven dependencies went away.
# ALL SIX COME FROM $ZPREFIX, not from the sysroot, and that is a decision with
# two measured reasons.
#
# First: Ubuntu builds libxcb-image.a and libxcb-util.a WITHOUT -fPIC, and
# they cannot go into a PIE program at all.
#
# Second, more important, concerns only the cursor but decides for all.
# libxcb-cursor has the theme directory list baked into the code, and the
# focal one is behind: it lacks ~/.local/share/icons, where GNOME and KDE put
# user themes today. Linking it in would give a program that silently fails to
# see the cursor theme. The other five are pure computational helpers (no
# baked-in paths, public API unchanged since 20.04) and are taken from upstream
# simply along with it, by one rule.
#
# packaging/linux/build-xcb-static.sh builds them; it also explains why
# --with-cursorpath is mandatory for the cursor.
set(zsys_own_lib "${ZDEPS_ROOT}/lib")

# The pairs are listed as foreach ARGUMENTS, not via a list variable: CMake
# expands ';' inside elements with `IN LISTS`, and the pairs collapse into a
# flat list (stepped on that right here).
foreach(pair "XCB_CURSOR_LIBRARY;${zsys_own_lib}/libxcb-cursor.a"
             "XCB_ICCCM_LIBRARY;${zsys_own_lib}/libxcb-icccm.a"
             "XCB_KEYSYMS_LIBRARY;${zsys_own_lib}/libxcb-keysyms.a"
             "XCB_RENDERUTIL_LIBRARY;${zsys_own_lib}/libxcb-render-util.a"
             "XCB_IMAGE_LIBRARY;${zsys_own_lib}/libxcb-image.a"
             "XCB_UTIL_LIBRARY;${zsys_own_lib}/libxcb-util.a"
             "XCB_ATOM_LIBRARY;${zsys_own_lib}/libxcb-util.a"
             "XCB_AUX_LIBRARY;${zsys_own_lib}/libxcb-util.a")
    list(GET pair 0 zsys_var)
    list(GET pair 1 zsys_path)
    if(EXISTS "${zsys_path}")
        set(${zsys_var} "${zsys_path}" CACHE FILEPATH "linked in statically" FORCE)
    else()
        message(WARNING
            "no ${zsys_path} — ${zsys_var} will stay dynamic.\n"
            "  Build it: packaging/linux/build-xcb-static.sh")
    endif()
endforeach()

# FIFTEEN OPTIONAL XCB EXTENSIONS: QUIET.
#
# Static Qt looks its dependencies up afresh for EVERY consumer and, among
# other things, calls `find_package(XCB 1.11)` WITHOUT a component list. By
# ECM convention that means "try every one you know", and the module dutifully
# prints fifteen lines of "Could NOT find XCB_COMPOSITE..." about extensions Qt
# does not ask us for at all (the thirteen it needs are listed by name and are
# all found).
#
# Half of them no longer exist (XEVIE and XPRINT were dropped from X.Org years
# ago); for the rest the sysroot holds only .so.0 without headers, so there is
# nowhere to report them to. These lines are thus permanent, affect nothing
# and only make a person doubt whether the build succeeded. Silenced by name:
# FPHSA is quiet when the package has <NAME>_FIND_QUIETLY set, and the names
# here are not real packages but what ECM calls its component check.
#
# WHAT IS NEEDED IS NOT SILENCED: if any of the thirteen required ones (CURSOR,
# ICCCM, IMAGE, KEYSYMS, RANDR, RENDER, RENDERUTIL, SHAPE, SHM, SYNC, UTIL,
# XFIXES, XKB) goes missing, configure fails just as loudly.
foreach(zsys_xcb_extra COMPOSITE DAMAGE DPMS DRI2 DRI3 GLX PRESENT RECORD RES
                       SCREENSAVER XEVIE XF86DRI XINERAMA XINPUT XPRINT XTEST XV XVMC)
    set(XCB_${zsys_xcb_extra}_FIND_QUIETLY TRUE)
endforeach()

# OpenMP WITH STATIC libgomp. Otherwise the finished program requires
# libgomp.so.1 (package libgomp1, priority=optional): usually present on a
# desktop, but "usually" is not the word one wants to hear about a program
# shipped as a single file. We link in what is ours, and OpenMP here is ours:
# the resampler and the color conversion in zametti-core are parallelized
# with it.
#
# The path is asked from the compiler, not written down: it depends on the gcc
# version, and a hand-written one would drift on the first sysroot update.
execute_process(COMMAND "${ZSYS_ROOT}/bin/gcc" -print-file-name=libgomp.a
                OUTPUT_VARIABLE zsys_libgomp
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(zsys_libgomp AND EXISTS "${zsys_libgomp}")
    set(OpenMP_gomp_LIBRARY "${zsys_libgomp}" CACHE FILEPATH "static libgomp" FORCE)
endif()

# The static libraries end up inside a PIE program.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
