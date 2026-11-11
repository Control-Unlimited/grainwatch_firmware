# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Force legacy driver headers to be visible globally
set(ESP_WM_COMPONENT_MODIFY "esp-idf-lib__i2cdev:REQUIRES:driver")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(control_unlimited)
