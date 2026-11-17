cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET esp32c3 CACHE STRING "ESP-IDF target")
if(CMAKE_HOST_WIN32)
    # Keep Arduino's include paths below the Windows process command-line limit.
    set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON)
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(weatherclock_voice)
