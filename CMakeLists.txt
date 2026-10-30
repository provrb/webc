cmake_minimum_required(VERSION 4.0.0)

# Link vcpkg deps statically on Windows. The default x64-windows triplet builds
# sqlite3 as a DLL, and clang rejects sqlite_orm's constexpr use of dllimport
# function addresses. Must be set before project().
if(CMAKE_HOST_WIN32 AND NOT DEFINED VCPKG_TARGET_TRIPLET)
  set(VCPKG_TARGET_TRIPLET x64-windows-static-md)
endif()

project(webc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Boost CONFIG REQUIRED COMPONENTS asio beast url)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(lexbor CONFIG REQUIRED)

file(GLOB SOURCES "src/*.cpp")
file(GLOB HEADERS "include/*.h")

add_executable(main ${SOURCES} ${HEADERS})

target_include_directories(main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(main PRIVATE Boost::asio Boost::beast Boost::url
                                   unofficial::sqlite3::sqlite3
                                   OpenSSL::Crypto lexbor::lexbor_static)
