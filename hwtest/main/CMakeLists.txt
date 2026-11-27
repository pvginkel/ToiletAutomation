idf_component_register(SRCS "hwtest.c"
                       PRIV_REQUIRES console esp_driver_gpio esp_driver_i2c esp_driver_i2s
                                     esp_psram efuse spi_flash esp_hw_support esp_timer)
