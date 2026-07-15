idf_component_register(
    SRCS "esp_main.cpp" "../bench/bench.cpp" "../bench/celeste_bench.cpp"
    INCLUDE_DIRS "." "../bench" "../../../assets"
    REQUIRES z8lua esp_timer
)
