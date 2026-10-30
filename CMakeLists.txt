cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# = the display's remote_io.h NODE_PROJECT_NAME: its node-OTA check refuses other project names
project(CarHvacIoNode)
