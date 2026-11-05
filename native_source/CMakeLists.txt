cmake_minimum_required(VERSION 3.16)

project(pyandroidserial LANGUAGES CXX)
# build: Qt_6_11_1_for_Android_arm64_v8a_Debug

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core)

add_library(pyandroidserial SHARED
    pyandroidserial.cpp
    pyandroidserial.h
    pyandroidserial_bt.h
    pyandroidserial_bt.cpp
)

target_link_libraries(pyandroidserial
    PRIVATE Qt6::Core
)