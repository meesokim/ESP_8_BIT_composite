idf_component_register(SRCS "Adafruit_GFX.cpp" "ESP_8_BIT_GFX.cpp" "main.cpp" "ESP_8_BIT_composite.cpp" "Print.cpp"
                    INCLUDE_DIRS ".")
add_compile_options(-Wno-error=format= -Wno-format)