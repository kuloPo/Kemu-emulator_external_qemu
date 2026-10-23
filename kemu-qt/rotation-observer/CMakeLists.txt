set(KEMU_PREBUILTS "${ANDROID_QEMU2_TOP_DIR}/../../prebuilts")
if(CMAKE_HOST_WIN32)
    set(KEMU_JDK_HOST win)
    set(KEMU_SDK_HOST windows)
    set(KEMU_JAVA_SUFFIX .exe)
elseif(CMAKE_HOST_APPLE)
    if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(KEMU_JDK_HOST mac-arm64)
    else()
        set(KEMU_JDK_HOST mac)
    endif()
    set(KEMU_SDK_HOST macosx)
else()
    set(KEMU_JDK_HOST linux)
    set(KEMU_SDK_HOST linux)
endif()
set(KEMU_JDK "${KEMU_PREBUILTS}/studio/jdk/jdk17/${KEMU_JDK_HOST}")
if(CMAKE_HOST_APPLE)
    string(APPEND KEMU_JDK "/Contents/Home")
endif()
set(KEMU_SDK "${KEMU_PREBUILTS}/android-emulator-build/system-images/android-sdk")
set(KEMU_ROTATION_JAVA "${KEMU_JDK}/bin/java${KEMU_JAVA_SUFFIX}" CACHE FILEPATH "Rotation observer Java runtime")
set(KEMU_ROTATION_JAVAC "${KEMU_JDK}/bin/javac${KEMU_JAVA_SUFFIX}" CACHE FILEPATH "Rotation observer Java compiler")
set(KEMU_ROTATION_D8 "${KEMU_SDK}/${KEMU_SDK_HOST}/build-tools/36.1.0/lib/d8.jar" CACHE FILEPATH "Rotation observer dex compiler")
set(KEMU_ROTATION_ANDROID_JAR "${KEMU_SDK}/common/platforms/android-36/android.jar" CACHE FILEPATH "Rotation observer Android API")
set(KEMU_ROTATION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/RotationObserverDex.h")
add_custom_command(OUTPUT "${KEMU_ROTATION_HEADER}"
    COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build.py"
        --java "${KEMU_ROTATION_JAVA}" --javac "${KEMU_ROTATION_JAVAC}"
        --d8 "${KEMU_ROTATION_D8}" --android-jar "${KEMU_ROTATION_ANDROID_JAR}"
        --output "${KEMU_ROTATION_HEADER}"
    DEPENDS build.py RotationObserver.java IRotationWatcher.java
        "${KEMU_ROTATION_JAVA}" "${KEMU_ROTATION_JAVAC}"
        "${KEMU_ROTATION_D8}" "${KEMU_ROTATION_ANDROID_JAR}"
    VERBATIM)
add_custom_target(kemu-rotation-observer DEPENDS "${KEMU_ROTATION_HEADER}")
add_dependencies(aemu-ui-qt kemu-rotation-observer)
target_include_directories(aemu-ui-qt PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
