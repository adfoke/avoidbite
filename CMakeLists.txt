cmake_minimum_required(VERSION 3.16)

project(avoidbite
        VERSION 1.0.0
        DESCRIPTION "旱厕蹲坑算法 — squat-pit, avoid-the-bite concurrency simulation"
        LANGUAGES C)

# 未显式指定时默认 Release（原 Makefile 用 -O2）
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(avoidbite avoidbite.c)

target_compile_options(avoidbite PRIVATE -Wall -Wextra)
target_link_libraries(avoidbite PRIVATE Threads::Threads)

include(CPack)
