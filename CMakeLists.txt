cmake_minimum_required(VERSION 3.10)
project(sesx_convert)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Paths
set(PUGIXML_DIR "${CMAKE_SOURCE_DIR}/extern/pugixml" CACHE PATH "Path to zeux/pugixml checkout")
set(REACONVERT_DIR "${CMAKE_SOURCE_DIR}/extern/ReaConvert" CACHE PATH "Path to atmosfar/ReaConvert checkout")
set(WDL_DIR "${REACONVERT_DIR}/extern/WDL/WDL" CACHE PATH "Path to justinfrankel/WDL checkout")
set(REAPER_SDK_DIR "${REACONVERT_DIR}/extern/reaper-sdk" CACHE PATH "Path to justinfrankel/reaper-sdk checkout")

# Force macOS architecture when generating on Apple hosts.
if(APPLE)
  if(NOT DEFINED CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL "")
    # Prefer host processor (Rosetta terminal will report x86_64)
    set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}" CACHE STRING "" FORCE)
    message(STATUS "Apple architecture set from HOST_SYSTEM_PROCESSOR: ${CMAKE_HOST_SYSTEM_PROCESSOR}")
  endif()
endif()

# SWELL configuration for plugins
add_definitions(-DSWELL_PROVIDED_BY_APP)

if(MSVC)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_definitions(-D_CRT_NONSTDC_NO_WARNINGS)
    add_compile_options(/wd4267 /wd4838 /wd4244)
elseif(UNIX AND NOT APPLE)
    # Linux
    add_definitions(-D_GNU_SOURCE)
    add_definitions(-DNOMINMAX -DWDL_NO_DEFINE_MINMAX)
endif()

# pugixml (only for SesxParser)
add_library(pugixml STATIC ${PUGIXML_DIR}/src/pugixml.cpp)

# ReaConvert library (delegate to its own CMakeLists.txt)
add_subdirectory(${REACONVERT_DIR} build/extern/ReaConvert)

# Core library (Static) - only sesx-specific code
set(LIB_SOURCES
    src/SesxParser.cpp
    src/SesxWriter.cpp
)

if(APPLE)
    list(APPEND LIB_SOURCES
      ${WDL_DIR}/swell/swell-modstub.mm
    )
elseif(WIN32)
    list(APPEND LIB_SOURCES
        ${WDL_DIR}/win32_utf8.c
    )
elseif(UNIX)
    # Linux/Unix
    list(APPEND LIB_SOURCES
        ${WDL_DIR}/swell/swell-modstub-generic.cpp
    )
endif()

add_library(${PROJECT_NAME}-lib STATIC
    ${LIB_SOURCES}
)
target_include_directories(${PROJECT_NAME}-lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PUGIXML_DIR}/src
)
target_link_libraries(${PROJECT_NAME}-lib ReaConvert pugixml)

# REAPER Plugin (Shared)
set(PLUGIN_SOURCES
    src/main.cpp
    ${WDL_DIR}/filebrowse.cpp
)

add_library(${PROJECT_NAME} SHARED
    ${PLUGIN_SOURCES}
)
target_link_libraries(${PROJECT_NAME} ${PROJECT_NAME}-lib)

if(APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        PREFIX "reaper_"
    )
    # choose suffix based on chosen arch
    if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
      set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX "-x86_64.dylib")
    elseif(CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
      set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX "-arm64.dylib")
    else()
      # default to x64 suffix for unknown cases
      set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX "-arm64.dylib")
    endif()

    find_library(APPKIT_LIBRARY AppKit)
    find_library(FOUNDATION_LIBRARY Foundation)
    find_library(COCOA_LIBRARY Cocoa)
    target_link_libraries(${PROJECT_NAME}
        ${APPKIT_LIBRARY}
        ${FOUNDATION_LIBRARY}
        ${COCOA_LIBRARY}
    )
elseif(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        PREFIX "reaper_"
        SUFFIX "-x64.dll"
    )
    target_link_libraries(${PROJECT_NAME} comdlg32 shell32 shlwapi)
else()
    # Linux
    set_target_properties(${PROJECT_NAME} PROPERTIES
        PREFIX "reaper_"
        SUFFIX "-x86_64.so"
    )
    find_library(DL_LIBRARY dl)
    target_link_libraries(${PROJECT_NAME} ${DL_LIBRARY})
endif()

# Automated deployment
if(APPLE)
    set(REAPER_USER_PLUGINS "$ENV{HOME}/Library/Application Support/REAPER/UserPlugins")
elseif(WIN32)
    set(REAPER_USER_PLUGINS "$ENV{APPDATA}/REAPER/UserPlugins")
elseif(UNIX)
    set(REAPER_USER_PLUGINS "$ENV{HOME}/.config/REAPER/UserPlugins")
endif()

if(REAPER_USER_PLUGINS)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REAPER_USER_PLUGINS}"
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${REAPER_USER_PLUGINS}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        COMMENT "Copying ${PROJECT_NAME} to ${REAPER_USER_PLUGINS}"
        VERBATIM
    )
endif()
