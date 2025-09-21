cmake_minimum_required(VERSION 3.16)
project(FastMeleeMacro LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.2 COMPONENTS Widgets REQUIRED)
find_package(Threads REQUIRED)
qt_standard_project_setup()

qt_add_executable(FastMeleeMacro
    src/main.cpp
)

qt_add_resources(FastMeleeMacro "app_resources"
    PREFIX "/icons"
    FILES resources/app_icon.png
)

target_link_libraries(FastMeleeMacro PRIVATE Qt6::Widgets Threads::Threads)

if (WIN32)
    target_link_libraries(FastMeleeMacro PRIVATE user32)
endif()
