glib_compile_resources(RESOURCES
    source
        ../sntray.gresource.xml
)

#################
# Xfce Panel part
#################
set(VALA_FILES_XFCE xfce4-sntray-applet.vala)
if (ENABLE_XFCE)
    vala_precompile(VALA_C_XFCE sntray-xfce
        ${VALA_FILES_XFCE}
    PACKAGES
        gtk+-3.0
        libxfce4panel-2.0
        libxfconf-0
    CUSTOM_VAPIS
        ${CMAKE_BINARY_DIR}/src/vala-panel-sntray.vapi
        ${CMAKE_SOURCE_DIR}/src/sntray-backend.vapi
    OPTIONS
        --vapidir=${CMAKE_SOURCE_DIR}/vapi
        --vapidir=${CMAKE_CURRENT_BINARY_DIR}
        --target-glib=2.44
        --gresources=${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}.gresource.xml
    )
    #####
    # Plugin Library
    #####
    add_library (sntray-xfce MODULE
        ${VALA_C_XFCE}
        ${RESOURCES}
    )
    target_include_directories(sntray-xfce  PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(sntray-xfce  sntray-core PkgConfig::XFCE)
    add_custom_target(sntray-xfce-ide SOURCES ${VALA_FILES_XFCE})

    # Install plugin stuffs
    install(TARGETS sntray-xfce DESTINATION ${CMAKE_INSTALL_LIBDIR}/xfce4/panel/plugins)
endif()


#################
# Vala Panel part
#################
set(VALA_FILES_VALAPANEL vala-panel-sntray-applet.vala)
if (ENABLE_VALAPANEL)
    if(NEW_IFACE)
        set(PLUGIN_PACKAGE "")
    else()
        set(PLUGIN_PACKAGE libpeas-1.0)
    endif()
    vala_precompile(VALA_C_VALAPANEL sntray-valapanel
            ${VALA_FILES_VALAPANEL}
    PACKAGES
        gtk+-3.0
        ${PLUGIN_PACKAGE}
        vala-panel
    CUSTOM_VAPIS
        ${CMAKE_BINARY_DIR}/src/vala-panel-sntray.vapi
        ${CMAKE_SOURCE_DIR}/src/sntray-backend.vapi
    OPTIONS
        --vapidir=${CMAKE_CURRENT_SOURCE_DIR}/vapi
        --vapidir=${CMAKE_CURRENT_BINARY_DIR}
        --target-glib=2.44
        --gresources=${CMAKE_SOURCE_DIR}/${PLUGIN_NAME}.gresource.xml
        ${NEW_DEFINE}
    )
    #####
    # Plugin Library
    #####
    add_library (sntray-valapanel MODULE
        ${VALA_C_VALAPANEL}
        ${RESOURCES}
    )
    target_include_directories(sntray-valapanel  PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(sntray-valapanel sntray-core PkgConfig::VALAPANEL)
    add_custom_target(sntray-valapanel-ide SOURCES ${VALA_FILES_VALAPANEL})
    set_target_properties(sntray-valapanel PROPERTIES
        OUTPUT_NAME sntray)

    # Install plugin stuffs
    install(TARGETS sntray-valapanel DESTINATION ${CMAKE_INSTALL_LIBDIR}/vala-panel/applets)
endif()

#################
# Budgie Panel part
#################
set(VALA_FILES_BUDGIE sntray-applet-budgie.vala)
if (ENABLE_BUDGIE)
    vala_precompile(VALA_C_BUDGIE sntray-budgie
            ${VALA_FILES_BUDGIE}
    PACKAGES
        gtk+-3.0
        budgie-1.0
    CUSTOM_VAPIS
        ${CMAKE_BINARY_DIR}/src/vala-panel-sntray.vapi
        ${CMAKE_SOURCE_DIR}/src/sntray-backend.vapi
    OPTIONS
        --vapidir=${CMAKE_CURRENT_SOURCE_DIR}/vapi
        --vapidir=${CMAKE_CURRENT_BINARY_DIR}
        --target-glib=2.44
        --gresources=${CMAKE_SOURCE_DIR}/${PLUGIN_NAME}.gresource.xml
    )
    #####
    # Plugin Library
    #####
    add_library (sntray-budgie MODULE
        ${VALA_C_BUDGIE}
        ${RESOURCES}
    )
    target_include_directories(sntray-budgie  PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(sntray-budgie sntray-core PkgConfig::BUDGIE)
    add_custom_target(sntray-budgie-ide SOURCES ${VALA_FILES_BUDGIE})

    # Install plugin stuffs
    install(TARGETS sntray-budgie DESTINATION ${CMAKE_INSTALL_LIBDIR}/budgie-desktop/plugins/budgie-sntray-plugin)
endif()

#################
# MATE Panel part
#################
set(VALA_FILES_MATE mate-sntray-applet.vala)
if (ENABLE_MATE)
    vala_precompile(VALA_C_MATE sntray-mate
        ${VALA_FILES_MATE}
    PACKAGES
        gdk-3.0
        gtk+-3.0
        libmatepanelapplet-4.0
    CUSTOM_VAPIS
        ${CMAKE_BINARY_DIR}/src/vala-panel-sntray.vapi
        ${CMAKE_SOURCE_DIR}/src/sntray-backend.vapi
    OPTIONS
        --vapidir=${CMAKE_SOURCE_DIR}/vapi
        --vapidir=${CMAKE_CURRENT_BINARY_DIR}
        --target-glib=2.44
        --gresources=${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}.gresource.xml
    )
    #####
    # Plugin Library
    #####
    add_library (sntray-mate MODULE
        ${VALA_C_MATE}
        ${RESOURCES}
    )
    target_include_directories(sntray-mate  PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_dependencies(sntray-mate sntray-core)
    target_link_libraries(sntray-mate sntray-core PkgConfig::MATE)
    add_custom_target(sntray-mate-ide SOURCES ${VALA_FILES_MATE})

    # Install plugin stuffs
    install(TARGETS sntray-mate DESTINATION ${CMAKE_INSTALL_LIBDIR}/mate-panel)
endif()
