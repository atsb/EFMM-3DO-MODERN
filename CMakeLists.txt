cmake_minimum_required(VERSION 3.24)
project(EFMM_3DO_SDL3 C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_package(SDL3 REQUIRED CONFIG)

set(EFMM_GAME_SOURCES
    ctst.c
    rend.c
    shoot.c
    objects.c
    ob_zombie.c
    ob_george.c
    ob_head.c
    ob_spider.c
    ob_powerup.c
    ob_exit.c
    ob_trigger.c
    clip.c
    levelfile.c
    leveldef.c
    genmessage.c
    statscreen.c
    thread.c
    titleseq.c
    imgfile.c
    loadloaf.c
    file.c
    timing.c
    sound.c
    soundinterface.c
    cinepak.c
    map.c
    option.c
    elkabong.c
    sdl3_3do.c
    operafs.c
    font.c
    misc.c
    project.c
)

add_executable(efmm ${EFMM_GAME_SOURCES})
target_include_directories(efmm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/FontStuff
)

target_link_libraries(efmm PRIVATE SDL3::SDL3)
target_compile_definitions(efmm PRIVATE
    EFMM_SDL3_PORT=1
    EFMM_3DO_SOFTWARE_CEL=1
)

if(MSVC)
    target_compile_options(efmm PRIVATE /W3)
else()
    target_compile_options(efmm PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-old-style-definition)
endif()

set_target_properties(efmm PROPERTIES OUTPUT_NAME "efmm")

if(UNIX AND NOT APPLE)
    target_link_libraries(efmm PRIVATE m)
endif()
