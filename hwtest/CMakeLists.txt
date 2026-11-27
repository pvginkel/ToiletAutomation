# Hardware sanity test for the ToiletAutomation board: a standalone ESP-IDF app, not the firmware.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hwtest)
