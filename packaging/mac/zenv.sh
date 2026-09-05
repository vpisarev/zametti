# Shared environment for the portable zametti build on macOS. Use it like this:
#
#   source packaging/mac/zenv.sh
#
# There are exactly three scripts here (owner's decision): zenv.sh (this file),
# build-qt.sh (static Qt: qtbase + qtsvg) and build-app.sh (the program itself,
# from cmake to Zametti.app and dmg). Both build scripts source zenv.sh: Qt and
# the program are built with THE SAME flags — Qt and the program drifting apart
# in their flags is the classic way to get a binary that builds but does not
# start.
#
# HOW THE MAC DIFFERS FROM LINUX, AND WHY THERE IS NO SYSROOT HERE. On Linux
# portability hinges on glibc, which has to come from the sysroot of an old
# Ubuntu. On the mac the "floor" is something ELSE: the SDK and the system
# version named by -mmacosx-version-min. macOS provides backward compatibility
# itself, and the system frameworks (AppKit, Metal, ImageIO) exist on every
# machine by construction — bundling them is neither needed nor allowed.
#
# Paths can be overridden from outside: ZDEPS=/other source packaging/mac/zenv.sh
#
# DIRECTORY LAYOUT — the owner's word (30.08.2026): nothing is written directly
# into ~/ai/work; dependencies live in ~/ai/work/zdeps with per-architecture
# subfolders (Qt prefixes), their sources and build trees in zdeps/build;
# experiments go to ~/ai/work/zametti-playground.

export ZDEPS="${ZDEPS:-$HOME/ai/work/zdeps}" # Qt prefixes: $ZDEPS/<arch>
export ZBUILD="${ZBUILD:-$ZDEPS/build}"      # Qt sources and build trees (safe to wipe)

# SYSTEM FLOOR. Anything older will not run the program; anything newer will.
# 13.0 (Ventura) is the minimum Qt 6.10 itself declares (qtbase/.cmake.conf,
# QT_SUPPORTED_MIN_MACOS_VERSION "13"). Below that Qt builds SILENTLY — it does
# not check the floor, only the SDK and Xcode — but nobody promises it works
# on a live macOS 12, and there is nothing to test it on; promising more than
# Qt promises would be dishonest (the fork was settled with the owner,
# 30.08.2026).
export ZMACOS_MIN="${ZMACOS_MIN:-13.0}"

# ARCHITECTURES — THE LIST OF WHAT WE BUILD; EXACTLY ONE PER CONFIGURE.
#
# The temptation to hand cmake CMAKE_OSX_ARCHITECTURES='arm64;x86_64' and get
# a universal binary in one pass is strong, and it is FALSE. Four vendored
# libraries pick their SIMD BEFORE the build, from CMAKE_SYSTEM_PROCESSOR:
# libgav1, libsodium, highway and libjxl. That variable has ONE value, while a
# universal build compiles both halves in one pass — so on an Apple Silicon
# mac all four would pick the ARM path for x86_64 as well.
#
# How that ends is recorded in 3rdparty/libgav1/CMakeLists.txt in the owner's
# own words: a file that does not get its own flag compiles to nothing —
# silently, without a single warning — and the library just turns out slower.
# So a universal binary would build and run, with half of it quietly degraded.
# Such a flag is worse than no flag at all.
#
# Hence both build scripts loop `for arch in $ZARCHS`, and every pass is ITS
# OWN configure with its own correct CMAKE_SYSTEM_PROCESSOR and its own SIMD
# flags; the universal binary is glued by lipo in build-app.sh. The list can
# be narrowed to one architecture from outside (iterating, fixing one half):
# ZARCHS=arm64 bash packaging/mac/build-app.sh
export ZARCHS="${ZARCHS:-arm64 x86_64}"

# The machine's native architecture. On the foreign one (for us: x86_64 on
# Apple Silicon) build-app.sh sets CMAKE_SYSTEM_PROCESSOR explicitly, otherwise
# cmake leaves the host arch in it and the SIMD selection above goes wrong;
# the build itself runs under Rosetta.
export ZHOSTARCH="$(uname -m)"

echo "zenv(mac): deps=$ZDEPS/<arch> build=$ZBUILD min=$ZMACOS_MIN archs=[$ZARCHS] host=$ZHOSTARCH"
