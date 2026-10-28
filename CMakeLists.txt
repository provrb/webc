cmake_minimum_required(VERSION 4.0.0)

project(webc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Boost CONFIG REQUIRED)

file(GLOB SOURCES "src/*.cpp")
file(GLOB HEADERS "src/hdr/*.h")

add_executable(main ${SOURCES} ${HEADERS})

target_link_libraries(main PRIVATE Boost::headers)