# GBL hardware drivers: power path (Q1, relay, sequences), PDB voltage sensing, power switch.
idf_component_register(SRCS "gbl_power.c" "gbl_vsens.c" "gbl_switch.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio esp_timer esp_adc)
