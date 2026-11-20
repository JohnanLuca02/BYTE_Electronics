# GBL EMERGENCY firmware - TEMPORARY, NOT the production firmware.
#
# Partial pre-charge: the relay closes even when the charge stalls (motors
# connected on the current board revision). Reuses the production drivers
# unchanged; the production project (../GBL_Firmware) knows nothing about it.
# Delete this whole folder once the pre-charge stage is redesigned.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../GBL_Firmware/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(gbl_firmware_emergency)
