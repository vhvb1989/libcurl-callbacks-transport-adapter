
cmake_minimum_required(VERSION 3.10)
project (my-transport-adapter LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 14)

# Find blobs lib
find_package(azure-storage-blobs-cpp REQUIRED)
find_package(CURL REQUIRED)

add_executable (
    my-transport
    src/main.cpp
    src/my_transport.cpp
    src/my_transport.hpp
)

target_link_libraries(my-transport PRIVATE Azure::azure-storage-blobs CURL::libcurl)