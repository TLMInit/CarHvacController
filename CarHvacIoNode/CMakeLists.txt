cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# One repo holds both boards: take the version only from this board's tags (git describe would
# otherwise pick the nearest tag of either board)
execute_process(COMMAND git describe --tags --dirty --always --match "v*-node*"
                WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE PROJECT_VER OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
# = the display's remote_io.h NODE_PROJECT_NAME: its node-OTA check refuses other project names
project(CarHvacIoNode)
