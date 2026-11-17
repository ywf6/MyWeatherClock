idf_component_register(SRCS "main.cpp" "bridge.cpp" "audio.cpp"
 INCLUDE_DIRS "." "../generated"
 PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_driver_i2s esp_driver_uart esp_timer app_update json mbedtls esp_websocket_client esp_audio_codec)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
