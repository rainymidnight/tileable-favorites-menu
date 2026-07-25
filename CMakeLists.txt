cmake_minimum_required(VERSION 3.21)

project(TileableFavoritesMenu VERSION 1.0.0.0 LANGUAGES CXX)

set(OUTPUT_FOLDER "" CACHE PATH "Optional Skyrim Data folder or mod-manager mod root")

if(MSVC)
    # Keep local checkout paths out of __FILE__, source_location, and debug metadata.
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" TFM_SOURCE_ROOT_NATIVE)
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:/experimental:deterministic>"
        "$<$<COMPILE_LANGUAGE:CXX>:/pathmap:${TFM_SOURCE_ROOT_NATIVE}=.>"
    )
endif()

set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    CommonLibVR
    GIT_REPOSITORY https://github.com/alandtse/CommonLibVR.git
    GIT_TAG bebf622adb93624a53a5b39bba410565d832b15e # v4.35.2
    GIT_SHALLOW FALSE
    GIT_SUBMODULES extern/openvr
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(CommonLibVR)

get_target_property(COMMONLIB_SRC_DIR CommonLibSSE SOURCE_DIR)
include("${COMMONLIB_SRC_DIR}/cmake/CommonLibSSE.cmake")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    @ONLY
)

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    src/plugin.cpp
    src/Actions.cpp
    src/Config.cpp
    src/Favorites.cpp
    src/IconLayer.cpp
    src/Layout.cpp
    src/PreviewCache.cpp
    src/Serialization.cpp
    src/SettingsMenu.cpp
    src/UI.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/skse-menu-framework"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/mesh-rendering-framework"
)
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /Zc:preprocessor /EHsc)
endif()

if(OUTPUT_FOLDER)
    set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")
    set(LICENSE_FOLDER "${OUTPUT_FOLDER}/licenses/TileableFavoritesMenu")
    add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${LICENSE_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/package" "${OUTPUT_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${LICENSE_FOLDER}/TileableFavoritesMenu-MIT.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/external/licenses" "${LICENSE_FOLDER}"
        VERBATIM
    )
endif()
