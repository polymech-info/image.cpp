cmake_minimum_required(VERSION 3.20)
project(pm-image-consumer LANGUAGES CXX)

find_package(pm-image CONFIG REQUIRED)
add_executable(pm-image-consumer main.cpp)
target_compile_features(pm-image-consumer PRIVATE cxx_std_20)
target_link_libraries(pm-image-consumer PRIVATE pm::image)
