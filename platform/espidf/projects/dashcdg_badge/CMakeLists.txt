# dashcdg_badge — ESP-IDF + FreeRTOS bring-up / future conference-badge receiver shell.
# https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/
cmake_minimum_required(VERSION 3.16)

# PROJECT_VER → esp_app_desc.version (home UI sysinfo "fw_ver"). Field is 32 bytes incl. NUL.
set(_dashcdg_badge_fw "unknown")
execute_process(
    COMMAND git -C "${CMAKE_SOURCE_DIR}" rev-parse --short=10 HEAD
    OUTPUT_VARIABLE _dashcdg_badge_git
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _dashcdg_badge_git_st)
if(_dashcdg_badge_git_st EQUAL 0 AND _dashcdg_badge_git)
    set(_dashcdg_badge_fw "${_dashcdg_badge_git}")
endif()
execute_process(
    COMMAND git -C "${CMAKE_SOURCE_DIR}" diff-index --quiet HEAD --
    RESULT_VARIABLE _dashcdg_badge_dirty
    ERROR_QUIET
    OUTPUT_QUIET)
if(NOT _dashcdg_badge_dirty EQUAL 0)
    string(APPEND _dashcdg_badge_fw "*")
endif()
string(SUBSTRING "${_dashcdg_badge_fw}" 0 31 _dashcdg_badge_fw31)
set(PROJECT_VER "${_dashcdg_badge_fw31}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dashcdg_badge)
