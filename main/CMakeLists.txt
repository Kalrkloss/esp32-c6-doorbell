idf_component_register(
    SRCS "app_main.cpp" "doorbell_button.cpp" "doorbell_device.cpp"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash driver
)
