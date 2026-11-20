cmake_minimum_required(VERSION 3.24)
project(AstraProSMViewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(OpenCV REQUIRED COMPONENTS core imgproc videoio)

# OpenNI2 (orbbec-openni2 on Arch provides headers at /usr/include/openni2
# and libOpenNI2.so; no upstream CMake package exists).
find_library(OPENNI2_LIB OpenNI2 REQUIRED)
find_path(OPENNI2_INCLUDE_DIR OpenNI.h PATH_SUFFIXES openni2 REQUIRED)

add_executable(AstraProSMViewer
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/OpenNICamera.h
    src/OpenNICamera.cpp
    src/RgbCamera.h
    src/RgbCamera.cpp
    src/StreamView.h
    src/StreamView.cpp
)

target_include_directories(AstraProSMViewer PRIVATE ${OPENNI2_INCLUDE_DIR})
target_link_libraries(AstraProSMViewer PRIVATE
    Qt6::Widgets
    ${OPENNI2_LIB}
    opencv_core
    opencv_imgproc
    opencv_videoio
)
