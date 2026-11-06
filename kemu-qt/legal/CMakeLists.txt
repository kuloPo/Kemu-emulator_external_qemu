install(FILES licenses.html sources.html TRADEMARKS.txt DESTINATION legal COMPONENT kemu-legal)
install(FILES qt-sources.html Qt-6.5.3-THIRD-PARTY.txt QtWebEngine-6.5.3-Chromium-THIRD-PARTY.txt
    DESTINATION legal COMPONENT kemu-legal)
install(FILES ../LICENSE DESTINATION legal/licenses RENAME Kemu-GPL-2.0.txt COMPONENT kemu-legal)
foreach(license GPLv2 GPLv3 LGPLv21 LGPLv3 APACHE2)
    install(FILES "${CMAKE_CURRENT_LIST_DIR}/licenses/LICENSE.${license}"
        DESTINATION legal/licenses COMPONENT kemu-legal)
endforeach()
