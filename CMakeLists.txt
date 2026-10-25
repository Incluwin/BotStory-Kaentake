cmake_minimum_required(VERSION 3.14)
project(Kaentake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")

# Build external dependencies
add_subdirectory(external)

# Build injector and laucher
add_subdirectory(src)

# movement_keys against a real v83 client folder (PCOM.dll, UI.wz): -DMOVEMENTKEYS_TEST_CLIENT=<folder>
set(MOVEMENTKEYS_TEST_CLIENT "" CACHE PATH "v83 client folder for the movement_keys WZ test")
if(EXISTS "${MOVEMENTKEYS_TEST_CLIENT}/UI.wz")
    enable_testing()
    add_executable(movementkeys_wz_test test/movementkeys_wz_test.cpp src/movementkeys.cpp src/debug.cpp)
    target_link_libraries(movementkeys_wz_test PRIVATE WzLib Detours)
    target_include_directories(movementkeys_wz_test PRIVATE src)
    add_test(NAME movementkeys_wz COMMAND movementkeys_wz_test "${MOVEMENTKEYS_TEST_CLIENT}")
endif()