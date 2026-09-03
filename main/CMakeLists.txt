set(SOURCES "audio_codecs/audio_codec.cc"
            "audio_codecs/no_audio_codec.cc"
            "audio_processing/audio_debugger.cc"
            "audio_processing/passthrough_audio_processor.cc"
            "protocols/protocol.cc"
            "protocols/mqtt_protocol.cc"
            "protocols/websocket_protocol.cc"
            "mcp_server.cc"
            "system_info.cc"
            "application.cc"
            "ota.cc"
            "settings.cc"
            "background_task.cc"
            "main.cc"
            )

set(INCLUDE_DIRS "." "audio_codecs" "protocols" "audio_processing")

file(GLOB BOARD_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/boards/common/*.cc)
list(APPEND SOURCES ${BOARD_COMMON_SOURCES})
list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards/common)
list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards/DaaVoiceTerminal)

set(BOARD_TYPE "DaaVoiceTerminal")
set(LANG_DIR "zh-CN")

file(GLOB BOARD_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_TYPE}/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_TYPE}/*.c
)
list(APPEND SOURCES ${BOARD_SOURCES})

set(LANG_JSON "${CMAKE_CURRENT_SOURCE_DIR}/assets/${LANG_DIR}/language.json")
set(LANG_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang_config.h")

idf_component_register(SRCS ${SOURCES}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    WHOLE_ARCHIVE
                    REQUIRES nvs_flash
                             esp_event
                             driver
                             esp_adc
                             json
                             pthread
                             esp_app_format
                             app_update
                             spi_flash
                             efuse
                             78__esp-opus
                             78__esp-opus-encoder
                             78__esp-ml307
                             78__esp-wifi-connect
                             espressif__button
                    )

if(NOT BOARD_NAME)
    set(BOARD_NAME ${BOARD_TYPE})
endif()
target_compile_definitions(${COMPONENT_LIB}
                    PRIVATE BOARD_TYPE=\"${BOARD_TYPE}\" BOARD_NAME=\"${BOARD_NAME}\"
                    )

add_custom_command(
    OUTPUT ${LANG_HEADER}
    COMMAND python ${PROJECT_DIR}/scripts/gen_lang.py
            --input "${LANG_JSON}"
            --output "${LANG_HEADER}"
    DEPENDS
        ${LANG_JSON}
        ${PROJECT_DIR}/scripts/gen_lang.py
    COMMENT "Generating ${LANG_DIR} language config"
)

add_custom_target(lang_header ALL
    DEPENDS ${LANG_HEADER}
)

# 78/esp-opus-encoder 2.3.3 does not declare its opus dependency.
idf_component_get_property(opus_encoder_lib 78__esp-opus-encoder COMPONENT_LIB)
if(opus_encoder_lib)
    target_link_libraries(${opus_encoder_lib} PUBLIC idf::78__esp-opus)
endif()
