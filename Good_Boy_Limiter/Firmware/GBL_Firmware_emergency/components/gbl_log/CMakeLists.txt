# GBL logging: black-box log file in flash + live log stream on the J2 UART.
idf_component_register(SRCS "gbl_log.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES spiffs esp_ringbuf esp_driver_uart esp_driver_gpio)
