cmake_minimum_required(VERSION 3.22)
project(KemuLegalTests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
find_program(KEMU_LRELEASE_EXECUTABLE NAMES lrelease lrelease-qt6 lrelease6 REQUIRED)
get_filename_component(KEMU "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(KEMU_TRANSLATION_QM "${CMAKE_CURRENT_BINARY_DIR}/kemu_zh_CN.qm")
add_custom_command(OUTPUT "${KEMU_TRANSLATION_QM}"
    COMMAND "${KEMU_LRELEASE_EXECUTABLE}" -nounfinished "${KEMU}/translations/kemu_zh_CN.ts" -qm "${KEMU_TRANSLATION_QM}"
    DEPENDS "${KEMU}/translations/kemu_zh_CN.ts" VERBATIM)
configure_file("${KEMU}/translations/translations.qrc.in" kemu_translations.qrc @ONLY)
add_custom_target(legal-translations DEPENDS "${KEMU_TRANSLATION_QM}")
file(GLOB LZMA_SOURCES CONFIGURE_DEPENDS "${KEMU}/third_party/lzma/*.c")
add_library(legal-lzma STATIC ${LZMA_SOURCES})
target_compile_definitions(legal-lzma PRIVATE Z7_ST)
target_include_directories(legal-lzma PUBLIC "${KEMU}/third_party/lzma")
add_executable(Kemu LegalInfoTests.cpp "${KEMU}/src/LegalInfoDialog.cpp"
    "${KEMU}/src/UpdateController.cpp" "${KEMU}/src/StorageMoveDialog.cpp"
    "${KEMU}/src/UiLanguage.cpp" "${KEMU}/update/UpdateCore.cpp"
    "${KEMU}/update/UpdateDownload.cpp" "${KEMU}/update/UpdateTransaction.cpp"
    "${KEMU}/resources/kemu.qrc" "${CMAKE_CURRENT_BINARY_DIR}/kemu_translations.qrc")
add_dependencies(Kemu legal-translations)
target_compile_definitions(Kemu PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(Kemu PRIVATE /utf-8)
target_include_directories(Kemu PRIVATE "${KEMU}/../android/third_party/json/single_include")
target_link_libraries(Kemu PRIVATE Qt6::Widgets legal-lzma bcrypt winhttp)
add_subdirectory("${KEMU}/legal" legal)
