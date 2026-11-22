# The bench spike's crashing firmware: it boots, counts its boots in RTC memory, and calls
# abort() a few seconds later. Shows whether a panic's output survives the re-enumeration.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bench_spike_abort)
