# Tab5 SD-card firmware updater. Lives in the `ota_0` slot (0xC20000, 1 MB).
# Build:  idf.py -C updater set-target esp32p4 && idf.py -C updater build
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "1.2.2")  # MD controls/audio/exit with PSRAM video staging
# Only what main/ needs (it pulls gwenesis and the IDF drivers in through its requirements);
# keeps unrelated IDF components such as cmock out of the build.
set(COMPONENTS main)
project(tab5_updater)
