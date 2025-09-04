cmake_minimum_required(VERSION 3.10)
project(joycon2cpp)

set(CMAKE_CXX_STANDARD 17)

# Linux-specific packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(BLUEZ REQUIRED bluez)
pkg_check_modules(UDEV REQUIRED libudev)

# Optional: SDL2 for controller output
find_package(SDL2)

# Add your source files
add_executable(joycon2cpp
    src/main.cpp
    src/bluetooth_linux.cpp
    src/virtual_controller_linux.cpp
    # Add other source files
)

# Include directories
target_include_directories(joycon2cpp PRIVATE
    ${BLUEZ_INCLUDE_DIRS}
    ${UDEV_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(joycon2cpp
    ${BLUEZ_LIBRARIES}
    ${UDEV_LIBRARIES}
    pthread
    # Add hidapi if using it
    hidapi-hidraw
)

# Compiler flags
target_compile_options(joycon2cpp PRIVATE ${BLUEZ_CFLAGS_OTHER})
