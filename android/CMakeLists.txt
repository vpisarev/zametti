# The phone shell — step 0 of the Android port (docs/zametti-brief-android0.md).
#
# QWidgets, deliberately minimal and deliberately disposable: a list of notes,
# a folder dialog and the reading view, enough to prove that the core and the
# renderer build, run and perform on a real device, and to take the numbers
# the QML decision will be made on. Nothing here is shared with app/main.cpp;
# what the phone reuses it takes from zametti-ui as a library (the panels, the
# tree, the list, the book view) and from the core.
#
# qt_add_executable on Android produces a MODULE library
# (libzametti-android_arm64-v8a.so) that QtActivity loads; the APK is made by
# the zametti-android_make_apk target (androiddeployqt + gradle), see
# packaging/android/build-app.sh.

# The Qt modules the program ends up with, named HERE as well: androiddeployqt
# learns what to pack by walking the link graph from this directory, and an
# imported target found by a subdirectory (Qt6::Network comes through
# zametti-core) is invisible to it — "mentioned as a dependency but not
# declared", and libQt6Network with its TLS plugin would stay out of the APK.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg Network)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

qt_add_executable(zametti-android MANUAL_FINALIZATION
    main.cpp
    mobile_window.h
    mobile_window.cpp
    note_list_page.h
    note_list_page.cpp
    folder_tree_dialog.h
    folder_tree_dialog.cpp
    note_page.h
    note_page.cpp
    perf_log.h
    perf_log.cpp
    android.qrc
)

# Qt6::Network is linked by the program itself and not only through the core
# (where it is PRIVATE): the plugin list of the APK — the TLS backend with
# OpenSSL inside, the network-information backend — is collected from the
# program's own link line, and a module that reaches it only as LINK_ONLY
# through two static libraries brings no plugins along.
target_link_libraries(zametti-android PRIVATE zametti-ui Qt6::Network zametti-code-rules)

# The manifest and resources under package/ are laid over Qt's template by
# androiddeployqt; the package name comes from HERE, not from a `package`
# attribute in the manifest (AGP 8 rejects that one). minSdk is the floor of
# the Qt libraries too (-android-ndk-platform in packaging/android/build-qt.sh).
set_target_properties(zametti-android PROPERTIES
    QT_ANDROID_PACKAGE_NAME       "io.zametti.zametti"
    QT_ANDROID_MIN_SDK_VERSION    35
    QT_ANDROID_TARGET_SDK_VERSION 36
    QT_ANDROID_VERSION_NAME       "${PROJECT_VERSION}"
    QT_ANDROID_VERSION_CODE       1
    QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/package"
)

# After ALL the properties: the finaliser reads them when it writes the
# deployment settings for androiddeployqt.
qt_finalize_target(zametti-android)
