android_add_library(
  TARGET netsim-wifi-connection
  LICENSE GPL-2.0-only
  SRC NetsimWifiConnection.cpp)
target_include_directories(netsim-wifi-connection PUBLIC ${ANDROID_QEMU2_TOP_DIR})
target_link_libraries(netsim-wifi-connection
                     PUBLIC grpc++ packet-streamer-proto-lib
                     PRIVATE android-emu-base)

android_add_test(
  TARGET netsim-wifi-connection_unittests
  SRC NetsimWifiConnection_unittest.cpp
  DEPS netsim-wifi-connection gmock_main)

android_add_library(
  TARGET android-wifi-service
  LICENSE Apache-2.0
  SRC # cmake-format: sortable
      ${ANDROID_QEMU2_TOP_DIR}/android-qemu2-glue/netsim/libslirp_driver.cpp
      ${ANDROID_QEMU2_TOP_DIR}/android-qemu2-glue/emulation/VirtioWifiForwarder.cpp
      ${ANDROID_QEMU2_TOP_DIR}/android-qemu2-glue/emulation/WifiService.cpp)

target_include_directories(android-wifi-service
                           PUBLIC ${ANDROID_QEMU2_TOP_DIR}
                           PRIVATE ${ANDROID_QEMU2_TOP_DIR}/include
                                   ${ANDROID_AUTOGEN})

target_compile_definitions(android-wifi-service PUBLIC -DNETSIM_WIFI)

android_target_compile_definitions(android-wifi-service windows PRIVATE "-DIOV_MAX=1")

target_link_libraries(
  android-wifi-service
  PUBLIC android-emu-base
         android-emu-net
  PRIVATE
         libslirp
         glib2
         ssl
         hostapd
         android-emu-base-headers)
