cmake_minimum_required(VERSION 3.22)

set(IDF_TARGET "esp32s3" CACHE STRING "Waveshare V2 target")
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components/esp32-ws154/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(inkboy)
