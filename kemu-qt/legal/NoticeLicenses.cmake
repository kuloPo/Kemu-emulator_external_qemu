# Use Kemu's complete license texts when generating component notices.
function(kemu_use_full_license_texts)
    get_property(notices GLOBAL PROPERTY LICENSES_LST)
    foreach(license GPLv2 GPLv3 LGPLv21 LGPLv3 APACHE2)
        foreach(root "${ANDROID_QEMU2_TOP_DIR}" INTERNAL)
            string(REPLACE
                "|${root}/LICENSES/LICENSE.${license}\n"
                "|${root}/kemu-qt/legal/licenses/LICENSE.${license}\n"
                notices "${notices}")
        endforeach()
    endforeach()
    set_property(GLOBAL PROPERTY LICENSES_LST "${notices}")
endfunction()

kemu_use_full_license_texts()
