# CANary firmware, developed and released with ESP-IDF 5.5.1. Build with:  idf.py build
cmake_minimum_required(VERSION 3.16)

file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" CANARY_VERSION)
string(STRIP "${CANARY_VERSION}" CANARY_VERSION)
set(PROJECT_VER "${CANARY_VERSION}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(canary)
