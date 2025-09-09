cmake_minimum_required(VERSION 3.16)
project(S57Processor CXX C)

list(APPEND CMAKE_PREFIX_PATH "/usr/local/gdal")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找 Boost 库
find_package(Boost 1.71 REQUIRED COMPONENTS program_options filesystem)

# 查找 GDAL 库
find_package(GDAL REQUIRED)

add_executable(export_nobjnm
  nobjnm.cpp
  miniz.c
  miniz.h
)

target_link_libraries(export_nobjnm
    PRIVATE
    Boost::program_options
    Boost::filesystem
    ${GDAL_LIBRARIES}
)

target_include_directories(export_nobjnm
    PRIVATE
    ${Boost_INCLUDE_DIRS}
    ${GDAL_INCLUDE_DIRS}
)

add_executable(export_depth
  depth.cpp
  miniz.c
  miniz.h
)

target_link_libraries(export_depth
    PRIVATE
    Boost::program_options
    Boost::filesystem
    ${GDAL_LIBRARIES}
)

target_include_directories(export_depth
    PRIVATE
    ${Boost_INCLUDE_DIRS}
    ${GDAL_INCLUDE_DIRS}
)
