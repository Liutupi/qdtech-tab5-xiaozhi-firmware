# Tab5 SD-card firmware updater. Lives in the `ota_0` slot (0xC20000, 1 MB).
# Build:  idf.py -C updater set-target esp32p4 && idf.py -C updater build
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "1.0.0")
project(tab5_updater)
