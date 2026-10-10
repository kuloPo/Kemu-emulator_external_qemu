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
