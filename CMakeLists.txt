cmake_minimum_required(VERSION 3.21)

project(MicrosurgicalGuidanceFilterViewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui videoio)

add_executable(MicrosurgicalGuidanceFilterViewer main.cpp)

target_include_directories(MicrosurgicalGuidanceFilterViewer PRIVATE
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(MicrosurgicalGuidanceFilterViewer PRIVATE
    ${OpenCV_LIBS}
)

if(MSVC)
    target_compile_options(MicrosurgicalGuidanceFilterViewer PRIVATE /W4)
else()
    target_compile_options(MicrosurgicalGuidanceFilterViewer PRIVATE -Wall -Wextra)
endif()
