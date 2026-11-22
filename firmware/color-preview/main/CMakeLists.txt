idf_component_register(
    SRCS "preview_main.c"
         "display.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_rom esp_adc
)

target_add_binary_data(${COMPONENT_LIB} "frames.bin" BINARY)
