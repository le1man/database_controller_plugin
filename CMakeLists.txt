cmake_minimum_required(VERSION 3.10)
project(database_controller_plugin LANGUAGES C CXX VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_FLAGS "-m32")
set(CMAKE_CXX_FLAGS "-m32")

add_subdirectory(sdk)
#add_subdirectory(network)
find_package(CURL REQUIRED)

include_directories(
	.
	pawn/source
	pawn/source/linux
	nlohmann
)

add_definitions(
	-DHAVE_STDINT_H=1
	-DPAWN_CELL_SIZE=32
)

add_library(${PROJECT_NAME} SHARED src/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE OMP-SDK CURL::libcurl)