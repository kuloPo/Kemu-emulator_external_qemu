# Qt Linguist compiles the maintained TS catalog; embed QM in both the UI and tests.
# Set KEMU_LRELEASE_EXECUTABLE when Linguist tools are not on PATH.
find_program(KEMU_LRELEASE_EXECUTABLE NAMES lrelease lrelease-qt6 lrelease6 REQUIRED)
find_program(KEMU_LUPDATE_EXECUTABLE NAMES lupdate lupdate-qt6 lupdate6)
set(KEMU_TRANSLATION_TS ${CMAKE_CURRENT_LIST_DIR}/translations/kemu_zh_CN.ts)
set(KEMU_TRANSLATION_QM ${CMAKE_CURRENT_BINARY_DIR}/kemu_zh_CN.qm)
add_custom_command(OUTPUT ${KEMU_TRANSLATION_QM}
    COMMAND ${KEMU_LRELEASE_EXECUTABLE} -nounfinished ${KEMU_TRANSLATION_TS} -qm ${KEMU_TRANSLATION_QM}
    DEPENDS ${KEMU_TRANSLATION_TS} VERBATIM)
configure_file(${CMAKE_CURRENT_LIST_DIR}/translations/translations.qrc.in
    ${CMAKE_CURRENT_BINARY_DIR}/kemu_translations.qrc @ONLY)
set(KEMU_TRANSLATION_CPP ${CMAKE_CURRENT_BINARY_DIR}/qrc_kemu_translations.cpp)
add_custom_command(OUTPUT ${KEMU_TRANSLATION_CPP}
    COMMAND Qt${QT_VERSION_MAJOR}::rcc --name kemu_translations
        ${CMAKE_CURRENT_BINARY_DIR}/kemu_translations.qrc -o ${KEMU_TRANSLATION_CPP}
    DEPENDS ${KEMU_TRANSLATION_QM} ${CMAKE_CURRENT_BINARY_DIR}/kemu_translations.qrc VERBATIM)
add_custom_target(kemu-translations DEPENDS ${KEMU_TRANSLATION_CPP})
set_source_files_properties(${KEMU_TRANSLATION_CPP} TARGET_DIRECTORY aemu-ui-qt PROPERTIES GENERATED TRUE)
add_dependencies(aemu-ui-qt kemu-translations)
set(KEMU_LANGUAGE_SOURCES ${CMAKE_CURRENT_LIST_DIR}/src/UiLanguage.cpp ${KEMU_TRANSLATION_CPP})
if(KEMU_LUPDATE_EXECUTABLE)
    add_custom_target(kemu-update-translations
        COMMAND ${KEMU_LUPDATE_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/src/UpdateController.cpp
            ${CMAKE_CURRENT_LIST_DIR}/src/WindowChrome.cpp
            ${CMAKE_CURRENT_LIST_DIR}/src/UiLanguage.cpp
            ${CMAKE_CURRENT_LIST_DIR}/src/PerformancePage.h
            ${CMAKE_CURRENT_LIST_DIR}/src/AndroidMute.cpp
            ${CMAKE_CURRENT_LIST_DIR}/src/StorageMoveDialog.cpp
            -no-obsolete -ts ${KEMU_TRANSLATION_TS}
        VERBATIM)
endif()

add_subdirectory(rotation-observer)

# Built as part of the emulator's existing Qt library and QApplication.
target_sources(aemu-ui-qt PRIVATE
    ${KEMU_LANGUAGE_SOURCES}
    ${CMAKE_CURRENT_LIST_DIR}/src/WindowChrome.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/WindowChrome.h
    ${CMAKE_CURRENT_LIST_DIR}/src/AndroidMute.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AndroidMute.h
    ${CMAKE_CURRENT_LIST_DIR}/src/AndroidRotation.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AndroidRotation.h
    ${CMAKE_CURRENT_LIST_DIR}/src/IconButton.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/IconButton.h
    ${CMAKE_CURRENT_LIST_DIR}/resources/kemu.qrc)
target_include_directories(aemu-ui-qt PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
option(KEMU_GUI_TESTS "Include opt-in live VM UI checks" OFF)
if(KEMU_GUI_TESTS AND WINDOWS_MSVC_X86_64)
    target_sources(aemu-ui-qt PRIVATE ${CMAKE_CURRENT_LIST_DIR}/tests/LiveGuiChecks.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/RotationGuiChecks.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/SensorRotationGuiChecks.cpp)
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/src/WindowChrome.cpp
        TARGET_DIRECTORY aemu-ui-qt PROPERTIES COMPILE_DEFINITIONS KEMU_GUI_TESTS=1)
endif()
if(WINDOWS_MSVC_X86_64)
    # Windows SDK import libraries; no third-party binary is redistributed.
    android_license(TARGET dwmapi user32 ws2_32 LIBNAME None SPDX None LICENSE None LOCAL None)
    target_link_libraries(aemu-ui-qt PRIVATE dwmapi)
    add_executable(Kemu WIN32 launcher/Main.cpp launcher/RuntimeConfig.cpp resources/kemu.rc)
    target_compile_features(Kemu PRIVATE cxx_std_17)
    target_compile_definitions(Kemu PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(Kemu PRIVATE resources)
    target_link_libraries(Kemu PRIVATE user32 ws2_32)
    set_target_properties(Kemu PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
    android_license(TARGET Kemu LIBNAME Kemu URL None SPDX GPL-2.0-only LICENSE None
        LOCAL "${CMAKE_CURRENT_LIST_DIR}/LICENSE")
    # Use the same install pipeline and destination as emulator.exe.
    android_install_exe(Kemu .)
    add_executable(kemu-config-tests tests/ConfigTests.cpp launcher/RuntimeConfig.cpp)
    target_compile_features(kemu-config-tests PRIVATE cxx_std_17)
    target_compile_definitions(kemu-config-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(kemu-config-tests PRIVATE launcher)
    set_target_properties(kemu-config-tests PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
endif()

if(WINDOWS_MSVC_X86_64)
    set(KEMU_JSON ${CMAKE_CURRENT_LIST_DIR}/../android/third_party/json/single_include)
    set(KEMU_ZLIB ${CMAKE_CURRENT_LIST_DIR}/../../zlib)
    add_library(kemu-update-zip STATIC
        ${KEMU_ZLIB}/contrib/minizip/unzip.c
        ${KEMU_ZLIB}/contrib/minizip/ioapi.c
        ${KEMU_ZLIB}/contrib/minizip/iowin32.c)
    target_include_directories(kemu-update-zip PUBLIC ${KEMU_ZLIB}/contrib/minizip)
    target_link_libraries(kemu-update-zip PUBLIC zlib)
    set_target_properties(kemu-update-zip PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
    android_license(TARGET kemu-update-zip LIBNAME minizip URL None SPDX Zlib LICENSE None LOCAL "${KEMU_ZLIB}/LICENSE")
    android_license(TARGET bcrypt winhttp shell32 libcmt libcpmt libvcruntime libucrt LIBNAME None SPDX None LICENSE None LOCAL None)
    target_sources(aemu-ui-qt PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src/StorageMoveDialog.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/UpdateController.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/UpdateController.h
        ${CMAKE_CURRENT_LIST_DIR}/update/UpdateCore.cpp
        ${CMAKE_CURRENT_LIST_DIR}/update/UpdateDownload.cpp
        ${CMAKE_CURRENT_LIST_DIR}/update/UpdateTransaction.cpp)
    target_include_directories(aemu-ui-qt PRIVATE ${KEMU_JSON})
    target_link_libraries(aemu-ui-qt PRIVATE kemu-update-zip bcrypt winhttp)
    add_executable(KemuUpdater WIN32 update/UpdaterMain.cpp update/UpdateCore.cpp update/UpdateTransaction.cpp resources/kemu.rc)
    target_compile_features(KemuUpdater PRIVATE cxx_std_17)
    target_compile_definitions(KemuUpdater PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    # The helper must not load runtime DLLs from the installation it replaces.
    target_compile_options(KemuUpdater PRIVATE /MT)
    target_include_directories(KemuUpdater PRIVATE ${KEMU_JSON} resources)
    target_link_libraries(KemuUpdater PRIVATE bcrypt user32 shell32 libcmt libcpmt libvcruntime libucrt)
    set_target_properties(KemuUpdater PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
    android_license(TARGET KemuUpdater LIBNAME Kemu URL None SPDX GPL-2.0-only LICENSE None LOCAL "${CMAKE_CURRENT_LIST_DIR}/LICENSE")
    android_install_exe(KemuUpdater .)
    add_executable(kemu-update-tests tests/UpdateTests.cpp update/UpdateCore.cpp update/UpdateTransaction.cpp update/UpdateDownload.cpp)
    target_compile_features(kemu-update-tests PRIVATE cxx_std_17)
    target_compile_definitions(kemu-update-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(kemu-update-tests PRIVATE ${KEMU_JSON} update)
    target_link_libraries(kemu-update-tests PRIVATE bcrypt winhttp kemu-update-zip)
    set_target_properties(kemu-update-tests PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
endif()

if(WINDOWS_MSVC_X86_64)
    add_executable(kemu-update-ui-tests ${KEMU_LANGUAGE_SOURCES} tests/UpdateUiTests.cpp src/UpdateController.cpp src/StorageMoveDialog.cpp resources/kemu.qrc update/UpdateCore.cpp update/UpdateTransaction.cpp update/UpdateDownload.cpp)
    add_dependencies(kemu-update-ui-tests kemu-translations)
    target_compile_features(kemu-update-ui-tests PRIVATE cxx_std_17)
    target_compile_definitions(kemu-update-ui-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(kemu-update-ui-tests PRIVATE ${KEMU_JSON})
    target_link_libraries(kemu-update-ui-tests PRIVATE Qt${QT_VERSION_MAJOR}::Widgets bcrypt winhttp kemu-update-zip)
    set_target_properties(kemu-update-ui-tests PROPERTIES AUTOMOC OFF AUTORCC ON AUTOUIC OFF)
    add_executable(kemu-restart-tests tests/RestartTests.cpp)
    add_executable(kemu-rotation-tests tests/RotationTests.cpp)
    target_compile_features(kemu-rotation-tests PRIVATE cxx_std_17)
    target_link_libraries(kemu-rotation-tests PRIVATE Qt${QT_VERSION_MAJOR}::Core)
    set_target_properties(kemu-rotation-tests PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
    target_compile_features(kemu-restart-tests PRIVATE cxx_std_17)
    target_compile_definitions(kemu-restart-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(kemu-restart-tests PRIVATE Qt${QT_VERSION_MAJOR}::Core ws2_32)
    set_target_properties(kemu-restart-tests PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
endif()

if(WINDOWS_MSVC_X86_64)
    add_executable(kemu-update-handoff-tests tests/UpdateHandoffTests.cpp update/UpdateCore.cpp)
    target_compile_features(kemu-update-handoff-tests PRIVATE cxx_std_17)
    target_compile_definitions(kemu-update-handoff-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(kemu-update-handoff-tests PRIVATE /MT)
    target_include_directories(kemu-update-handoff-tests PRIVATE ${KEMU_JSON} update)
    target_link_libraries(kemu-update-handoff-tests PRIVATE bcrypt libcmt libcpmt libvcruntime libucrt)
    set_target_properties(kemu-update-handoff-tests PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
endif()
