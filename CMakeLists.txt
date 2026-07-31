cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME AD9910)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/AD9910/ad9910.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/PLL/dual_adc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/PLL/phase_detector.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/PLL/phase_compensation.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/PLL/lock_controller.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/PLL/pll_demo.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/SCREEN_TJC/tjc_protocol_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/SCREEN_TJC/tjc_usart_hmi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/SCREEN_TJC/tjc_ctrl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/APP/app_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/APP/app_hmi_map.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/APP/app_board.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/APP/app_integration.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/TASK5/openmv_protocol.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/TASK5/openmv_uart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/TASK5/app_saw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/TASK5/task5_controller.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/AD9910
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/PLL
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/SCREEN_TJC
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/APP
    ${CMAKE_CURRENT_SOURCE_DIR}/Usr/TASK5
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
    TJC_UART_HANDLE=huart1
    TJC_UART_INSTANCE=USART1
    TJC_UART_RX_MODE=TJC_UART_MODE_IT
    TJC_UART_TX_MODE=TJC_UART_MODE_IT
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
