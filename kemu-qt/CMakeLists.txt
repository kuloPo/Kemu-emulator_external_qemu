# Built as part of the emulator's existing Qt library and QApplication.
target_sources(aemu-ui-qt PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src/WindowChrome.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/WindowChrome.h
    ${CMAKE_CURRENT_LIST_DIR}/src/AndroidMute.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AndroidMute.h
    ${CMAKE_CURRENT_LIST_DIR}/src/IconButton.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/IconButton.h
    ${CMAKE_CURRENT_LIST_DIR}/resources/kemu.qrc)
target_include_directories(aemu-ui-qt PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
option(KEMU_GUI_TESTS "Include opt-in live VM UI checks" OFF)
if(KEMU_GUI_TESTS AND WINDOWS_MSVC_X86_64)
    target_sources(aemu-ui-qt PRIVATE ${CMAKE_CURRENT_LIST_DIR}/tests/LiveGuiChecks.cpp)
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
    add_executable(kemu-update-ui-tests tests/UpdateUiTests.cpp src/UpdateController.cpp update/UpdateCore.cpp update/UpdateTransaction.cpp update/UpdateDownload.cpp)
    target_compile_features(kemu-update-ui-tests PRIVATE cxx_std_17)
    target_compile_definitions(kemu-update-ui-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(kemu-update-ui-tests PRIVATE ${KEMU_JSON})
    target_link_libraries(kemu-update-ui-tests PRIVATE Qt${QT_VERSION_MAJOR}::Widgets bcrypt winhttp kemu-update-zip)
    set_target_properties(kemu-update-ui-tests PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
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
