# GBL EMERGENCY firmware - TEMPORARY, NOT the production firmware.
#
# Partial pre-charge: the relay closes even when the charge stalls (motors
# connected on the current board revision). Self-contained copy: own drivers
# (components/) and partition table, nothing shared with ../GBL_Firmware.
# Delete this whole folder once the pre-charge stage is redesigned.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(gbl_firmware_emergency)
