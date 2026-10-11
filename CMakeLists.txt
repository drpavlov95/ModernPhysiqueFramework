# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.21)

project(MPF VERSION 0.1.0 LANGUAGES CXX)

# The startup log carries an exact source fingerprint. This catches stale or
# conflicting MPF.dll copies without depending on Git being installed on the
# machine doing the build. Reconfigure whenever an identity input changes.
set(MPF_BUILD_ID_FILES
    src/plugin.cpp
    src/tri.cpp
    src/tri.h
    src/bodytri.cpp
    src/bodytri.h
    src/ini_edit.cpp
    src/ini_edit.h
    src/menu.cpp
    src/menu.h
    src/third_party/skee/SKEE.h
    src/third_party/skse_menu_framework/API.h
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MPF_BUILD_ID_FILES})
set(MPF_BUILD_ID_INPUT "MPF-${PROJECT_VERSION}-direct-skee")
foreach(MPF_BUILD_ID_FILE IN LISTS MPF_BUILD_ID_FILES)
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${MPF_BUILD_ID_FILE}" MPF_FILE_HASH)
    string(APPEND MPF_BUILD_ID_INPUT "-${MPF_FILE_HASH}")
endforeach()
string(SHA256 MPF_BUILD_ID_FULL "${MPF_BUILD_ID_INPUT}")
string(SUBSTRING "${MPF_BUILD_ID_FULL}" 0 12 MPF_BUILD_ID)

# Deployment is opt-in and needs an explicit destination: a stale cache entry
# alone must never be enough to copy a DLL into the game.
option(RFM_ENABLE_POST_BUILD_DEPLOY "Allow CMake to copy the plugin after building" OFF)
set(RFM_DEPLOY_OUTPUT "" CACHE PATH "Deploy root folder (the one containing SKSE/Plugins)")

# CommonLibSSE's exported target links Microsoft::DirectXTK, so this comes first.
find_package(directxtk CONFIG REQUIRED)
find_package(CommonLibSSE CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES
        src/plugin.cpp
        src/tri.cpp
        src/tri.h
        src/bodytri.cpp
        src/bodytri.h
        src/ini_edit.cpp
        src/ini_edit.h
        src/menu.cpp
        src/menu.h
        src/third_party/skee/SKEE.h
        src/third_party/skse_menu_framework/API.h
)

target_include_directories(${PROJECT_NAME} PRIVATE src)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_link_libraries(${PROJECT_NAME} PRIVATE tomlplusplus::tomlplusplus)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    MPF_VERSION="${PROJECT_VERSION}"
    MPF_BUILD_ID="${MPF_BUILD_ID}"
    MPF_BODY_BACKEND="direct-RaceMenu/SKEE"
)
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
endif()

# SimpleIni is header-only: the user-facing NPC list is an INI, while the
# author-facing morph manifest stays TOML.
find_path(SIMPLEINI_INCLUDE_DIRS "SimpleIni.h")
target_include_directories(${PROJECT_NAME} PRIVATE ${SIMPLEINI_INCLUDE_DIRS})

if(RFM_ENABLE_POST_BUILD_DEPLOY)
    if(NOT RFM_DEPLOY_OUTPUT)
        message(FATAL_ERROR "RFM_ENABLE_POST_BUILD_DEPLOY requires an explicit RFM_DEPLOY_OUTPUT")
    endif()
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RFM_DEPLOY_OUTPUT}/SKSE/Plugins"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${PROJECT_NAME}>" "${RFM_DEPLOY_OUTPUT}/SKSE/Plugins/"
    )
endif()
