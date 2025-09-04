cmake_minimum_required(VERSION 3.10)
project(joycon2cpp VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set default build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pthread")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# Check for Bluetooth libraries
pkg_check_modules(BLUEZ bluez)
if(NOT BLUEZ_FOUND)
    message(WARNING "BlueZ not found. Bluetooth support will be disabled.")
endif()

# Check for udev
pkg_check_modules(UDEV libudev)
if(NOT UDEV_FOUND)
    message(WARNING "libudev not found.")
endif()

# Check for hidapi
pkg_check_modules(HIDAPI hidapi-hidraw)
if(NOT HIDAPI_FOUND)
    pkg_check_modules(HIDAPI hidapi-libusb)
    if(NOT HIDAPI_FOUND)
        pkg_check_modules(HIDAPI hidapi)
    endif()
endif()

# Optional: SDL2 for alternative controller output
find_package(SDL2)
if(SDL2_FOUND)
    message(STATUS "SDL2 found: ${SDL2_LIBRARIES}")
else()
    message(STATUS "SDL2 not found. SDL2 support will be disabled.")
endif()

# Check for evdev
pkg_check_modules(EVDEV libevdev)

# Collect source files
# Assuming your source files are in src/ directory
# Adjust these paths based on your actual file structure
set(SOURCES
    # Main application
    src/main.cpp
    
    # Core functionality
    src/controller_decoder.cpp
    src/controller_manager.cpp
    
    # Linux-specific implementations
    src/linux/bluetooth_manager.cpp
    src/linux/virtual_controller.cpp
    src/linux/hid_device.cpp
    
    # Controller types
    src/controllers/joycon_left.cpp
    src/controllers/joycon_right.cpp
    src/controllers/joycon_dual.cpp
    src/controllers/pro_controller.cpp
    src/controllers/gc_controller.cpp
    
    # Utilities
    src/utils/config.cpp
    src/utils/logger.cpp
)

# If source files don't exist yet, use a minimal set
# You can comment this out once you have the actual files
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
    message(WARNING "Source files not found. Creating minimal build configuration.")
    set(SOURCES
        # You'll need to create at least a main.cpp
        main.cpp
    )
endif()

# Create executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Set include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Add system include directories if found
if(BLUEZ_FOUND)
    target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${BLUEZ_INCLUDE_DIRS})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_BLUETOOTH=1)
endif()

if(UDEV_FOUND)
    target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${UDEV_INCLUDE_DIRS})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_UDEV=1)
endif()

if(HIDAPI_FOUND)
    target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${HIDAPI_INCLUDE_DIRS})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_HIDAPI=1)
endif()

if(SDL2_FOUND)
    target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_SDL2=1)
endif()

if(EVDEV_FOUND)
    target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${EVDEV_INCLUDE_DIRS})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_EVDEV=1)
endif()

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    Threads::Threads
)

if(BLUEZ_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${BLUEZ_LIBRARIES})
endif()

if(UDEV_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${UDEV_LIBRARIES})
endif()

if(HIDAPI_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${HIDAPI_LIBRARIES})
endif()

if(SDL2_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${SDL2_LIBRARIES})
endif()

if(EVDEV_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${EVDEV_LIBRARIES})
endif()

# Installation rules
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# Create udev rules file for installation
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/udev/99-joycon.rules.in"
    "${CMAKE_CURRENT_BINARY_DIR}/99-joycon.rules"
    @ONLY
)

# Optional: Install udev rules
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/99-joycon.rules"
    DESTINATION /etc/udev/rules.d
    COMPONENT udev-rules
)

# Print configuration summary
message(STATUS "")
message(STATUS "Configuration Summary:")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Bluetooth (BlueZ): ${BLUEZ_FOUND}")
message(STATUS "  HID API: ${HIDAPI_FOUND}")
message(STATUS "  SDL2: ${SDL2_FOUND}")
message(STATUS "  udev: ${UDEV_FOUND}")
message(STATUS "  evdev: ${EVDEV_FOUND}")
message(STATUS "")
message(STATUS "To build: make")
message(STATUS "To install: sudo make install")
message(STATUS "To install udev rules: sudo make install COMPONENT=udev-rules")
message(STATUS "")
