cmake_minimum_required(VERSION 3.16.0)
# PlatformIO discovers framework/src automatically, while a raw idf.py build
# only scans main/ and components/. Register the existing source directory so
# app_main and the firmware entry point are linked in CI as well.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/src")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firmware)
