cmake_minimum_required(VERSION 3.16)
project(parcore
  DESCRIPTION "Hardware Parquet De- and Encoder"
  VERSION 0.1.0
  LANGUAGES CXX
)

# Default to a Release build (optimized) unless the user specifies otherwise.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT TARGET Coyote)
    find_package(Coyote QUIET)
    if(NOT Coyote_FOUND)
        add_subdirectory(../libstf/coyote/sw coyote)
    endif()
endif()
if(NOT TARGET libstf)
    find_package(libstf QUIET)
    if(NOT libstf_FOUND)
        add_subdirectory(../libstf/software libstf)
    endif()
endif()

include(GNUInstallDirs)

add_library(parcore SHARED)

target_sources(parcore PRIVATE
    parcore/configuration.cpp
    parcore/column_chunk_decoder.cpp

    parcore/metadata/metadata.cpp
)
target_include_directories(parcore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/parcore>
)

target_link_libraries(parcore PRIVATE Coyote)
target_include_directories(parcore PRIVATE ${COYOTE_INCLUDE_DIRS})
target_link_libraries(parcore PRIVATE libstf)
target_include_directories(parcore PRIVATE ${LIBSTF_INCLUDE_DIRS})

# Install the library
install(TARGETS parcore
    EXPORT parcoreTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Install headers
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/parcore/"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/parcore
    FILES_MATCHING PATTERN "*.hpp"
)

# Export package configuration
install(EXPORT parcoreTargets
    FILE parcoreTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/parcore
)

# Generate CMake package configuration files
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/parcoreConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/parcoreConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/parcore
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/parcoreConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/parcoreConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/parcoreConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/parcore
)
