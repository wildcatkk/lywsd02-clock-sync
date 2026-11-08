idf_component_register(
    SRCS app_main.c store.c logs.c wifi.c web.c ota.c jobs.c cluster.c lywsd02.c ble_clock.c
         selftest.c console.c ntp.c
    INCLUDE_DIRS .
    PRIV_REQUIRES nvs_flash esp_app_format esp_partition app_update bootloader_support
                  esp_wifi esp_netif esp_event esp_http_server esp_http_client esp_timer dns_server bt
                  esp_driver_usb_serial_jtag esp_driver_uart)

# wifi.c fixes picolibc's reading of negative DST switch times.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=_tzset_unlocked")

# The page is the same file the Windows and Linux builds serve, gzipped at build
# time. mtime=0 keeps the output byte-identical between builds.
idf_build_get_property(python PYTHON)
set(repo_static "${CMAKE_CURRENT_LIST_DIR}/../../../static")
set(sources
    "${repo_static}/index.html" "${repo_static}/i18n.js" "${repo_static}/icon.png"
    "${repo_static}/timezones.js" "${repo_static}/zones.js" "${CMAKE_CURRENT_LIST_DIR}/config.html")
foreach(source ${sources})
    get_filename_component(asset "${source}" NAME)
    set(packed "${CMAKE_CURRENT_BINARY_DIR}/${asset}.gz")
    add_custom_command(
        OUTPUT "${packed}"
        COMMAND "${python}" -c
            "import gzip,sys;open(sys.argv[2],'wb').write(gzip.compress(open(sys.argv[1],'rb').read(),9,mtime=0))"
            "${source}" "${packed}"
        DEPENDS "${source}"
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} "${packed}" BINARY)
endforeach()
