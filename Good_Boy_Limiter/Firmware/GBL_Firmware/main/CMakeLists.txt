# Application layer. Both applications are always compiled; main.c starts the
# one selected in menuconfig ("GBL firmware" -> "Application").
idf_component_register(SRCS "main.c" "gbl_run.c" "gbl_debug.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES gbl_hw)
