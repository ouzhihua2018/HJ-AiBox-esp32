file(GLOB_RECURSE srcs
          "esphome/*.cpp")

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS "." "include"
                                 "include/esphome/core"
                                 "include/esphome/components/micro_wake_word"
                                 "include/esphome/components/microphone"
                                 "include/esphome/components/i2s_audio"
                                 "include/esphome/components/i2s_audio/microphone"
                    REQUIRES 
                             "espressif__esp-tflite-micro"
                             "ESPMicroSpeechFeatures"
                             "esp_common" 
                             "esp_driver_i2s"
                             "esp_system"
                             "esp_timer"
                             "freertos"
                             "log")

target_compile_definitions(${COMPONENT_LIB} PUBLIC USE_ESP32 USE_ESP_IDF)
if(NOT DEFINED USE_ESP32)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC USE_ESP32) 
endif()         #小心组件public依赖，会导致所有依赖该组件的组件在编译时都会带上编译选项-D USE_ESP32

if(NOT DEFINED USE_ESP_IDF)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC USE_ESP_IDF)
endif()
