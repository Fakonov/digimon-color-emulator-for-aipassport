cmake_minimum_required(VERSION 3.16)

# Single source of truth for the firmware version.  ESP-IDF copies PROJECT_VER
# into the app descriptor, so the running image reports it over the console
# (`V` prints it and the boot banner logs it) without a second hard-coded
# string that could drift out of sync.
file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" PROJECT_VER)
string(STRIP "${PROJECT_VER}" PROJECT_VER)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(digicore-c3)
