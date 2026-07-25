##
# @file CMakeLists.txt
# @brief Nightshift T5 firmware - project root build file
#/

# APP_PATH
set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})

# APP_NAME
get_filename_component(APP_NAME ${APP_PATH} NAME)

# APP_SRC
# Keep this list explicit so obsolete MVP modules cannot accidentally be
# linked back into the protocol UART or LVGL callback path.
set(APP_SRC
    ${APP_PATH}/src/firmware_main.c
    ${APP_PATH}/src/t5_protocol.c
    ${APP_PATH}/src/frame_stream.c
    ${APP_PATH}/src/request_cache.c
    ${APP_PATH}/src/link_session.c
    ${APP_PATH}/src/state_store.c
    ${APP_PATH}/src/command_router.c
    ${APP_PATH}/src/uart_transport.c
    ${APP_PATH}/src/panel_ui.c
)

# APP_INC
set(APP_INC ${APP_PATH}/include ${APP_PATH}/src)

# APP_OPTIONS
set(APP_OPTIONS "-W")
list(APPEND APP_OPTIONS "-Wall" "-DLV_LVGL_H_INCLUDE_SIMPLE")

########################################
# Target Configure
########################################
add_library(${EXAMPLE_LIB})
message(STATUS "EXAMPLE_LIB:${APP_PATH}")

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRC}
    )

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_INC}
    )

target_compile_options(${EXAMPLE_LIB}
    PRIVATE
        ${APP_OPTIONS}
    )
