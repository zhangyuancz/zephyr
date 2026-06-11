cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(l511c_bringup LANGUAGES C CXX)

# === C sources ===
target_sources(app PRIVATE src/main.c)

# === C++ OCPP bridge ===
set_source_files_properties(src/ocpp_bridge.cpp PROPERTIES LANGUAGE CXX)
target_sources(app PRIVATE src/ocpp_bridge.cpp)

# === C++ STL headers (overrides picolibc's -nostdinc++) ===
# Zephyr SDK's full C++ STL is available but blocked by -nostdinc++
# We add the include paths explicitly for C++ files only
get_filename_component(GNU_BINDIR ${CMAKE_CXX_COMPILER} DIRECTORY)
get_filename_component(GNU_PREFIX ${GNU_BINDIR} DIRECTORY)
set(SDK_SYSROOT ${GNU_PREFIX}/arm-zephyr-eabi)
set(CXX_HEADER_ROOT ${SDK_SYSROOT}/include/c++/14.3.0)
# Target-specific C++ headers (c++config.h etc.)
execute_process(COMMAND ${CMAKE_CXX_COMPILER} -mcpu=cortex-m33 -mthumb -mfp16-format=ieee -print-multi-directory
  OUTPUT_VARIABLE MULTI_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
target_include_directories(app SYSTEM PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:${CXX_HEADER_ROOT}>
  $<$<COMPILE_LANGUAGE:CXX>:${CXX_HEADER_ROOT}/${MULTI_DIR}>
  $<$<COMPILE_LANGUAGE:CXX>:${CXX_HEADER_ROOT}/arm-zephyr-eabi/${MULTI_DIR}>
)

# Link C++ standard library (MicroOcpp needs std::string, new/delete, static ctors)
zephyr_link_libraries(stdc++ supc++)

# === MicroOcpp ===
file(GLOB_RECURSE MO_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/lib/MicroOcpp/src/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/lib/MicroOcpp/src/*.c
)
# Exclude files that need mbedTLS directly, or conflict with our adapter
list(FILTER MO_SOURCES EXCLUDE REGEX "FtpMbedTLS|CertificateMbedTLS|CertificateService|Platform\\.cpp")

foreach(src ${MO_SOURCES})
  set_source_files_properties(${src} PROPERTIES LANGUAGE CXX)
  target_sources(app PRIVATE ${src})
endforeach()

# === Include paths ===
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/lib/MicroOcpp/src
  ${CMAKE_CURRENT_SOURCE_DIR}/lib/ArduinoJson/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# === Build flags for MicroOcpp ===
target_compile_definitions(app PRIVATE
  MO_CUSTOM_WS
  MO_CUSTOM_CONSOLE
  MO_CUSTOM_TIMER
  MO_CUSTOM_RNG
  MO_PLATFORM=0
  MO_ENABLE_V201
  MO_OVERRIDE_ALLOCATION=1
  MO_CUSTOM_CONSOLE_MAXMSGSIZE=512
)
