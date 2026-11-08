cmake_minimum_required(VERSION 3.22)

# One version for the whole repository: the firmware reports the same number as the
# Windows and Linux builds, so a mixed cluster is easy to read.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../../VERSION" PROJECT_VER LIMIT_COUNT 1)
string(STRIP "${PROJECT_VER}" PROJECT_VER)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(lywsd02-clock-sync)
