idf_component_register(SRCS "main.c" "gbl_run_emergency.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES gbl_hw gbl_log esp_timer)
