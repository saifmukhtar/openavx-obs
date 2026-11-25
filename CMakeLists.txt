cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/plugin-main.c src/openavx-source.c src/openavx-output.c src/openavx-filter.c src/openavx-filter.h
)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_FRONTEND_API)
  target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE src/openavx-output-controller.c src/openavx-output-controller.h
  )
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_QT)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
  if(ENABLE_FRONTEND_API)
    target_sources(
      ${CMAKE_PROJECT_NAME}
      PRIVATE src/openavx-output-dialog.cpp src/openavx-output-dialog.hpp
    )
  endif()
endif()

# ------------------------------------------------------------------------------
# OpenAVX Core Library Linkage & Runtime Bundling
# ------------------------------------------------------------------------------
set(
  OPENAVX_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../openavx"
  CACHE PATH
  "Path to OpenAVX core repository when not pre-installed"
)
set(OPENAVX_GIT_TAG "v0.2.2" CACHE STRING "Git tag used if OPENAVX_FETCH_GIT is enabled")
option(OPENAVX_FETCH_GIT "Fetch OpenAVX from GitHub if not installed or present in OPENAVX_DIR" ON)
option(OPENAVX_BUNDLE_RUNTIME "Copy/install libopenavx runtime library alongside plugin binary" ON)

find_package(openavx CONFIG QUIET)

if(NOT TARGET openavx::openavx)
  if(EXISTS "${OPENAVX_DIR}/CMakeLists.txt")
    set(BUILD_TOOLS OFF CACHE BOOL "Build OpenAVX CLI utilities" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "Build OpenAVX automated test suites" FORCE)
    add_subdirectory("${OPENAVX_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/openavx_sub" EXCLUDE_FROM_ALL)
  elseif(OPENAVX_FETCH_GIT)
    include(FetchContent)
    set(BUILD_TOOLS OFF CACHE BOOL "Build OpenAVX CLI utilities" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "Build OpenAVX automated test suites" FORCE)
    FetchContent_Declare(
      openavx
      GIT_REPOSITORY https://github.com/saifmukhtar/openavx.git
      GIT_TAG "${OPENAVX_GIT_TAG}"
      GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(openavx)
  else()
    message(
      FATAL_ERROR
      "libopenavx not found. Install OpenAVX (CMAKE_PREFIX_PATH), place core repo at '${OPENAVX_DIR}', "
      "pass -DOPENAVX_DIR=..., or enable -DOPENAVX_FETCH_GIT=ON."
    )
  endif()
endif()

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE openavx::openavx)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(APPLE)
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES
      BUILD_RPATH "@loader_path;@loader_path/../Frameworks;$<TARGET_FILE_DIR:openavx::openavx>"
      INSTALL_RPATH "@loader_path;@loader_path/../Frameworks"
  )
elseif(UNIX)
  if(OPENAVX_BUNDLE_RUNTIME)
    set(_install_rpath "$ORIGIN")
  else()
    set(_install_rpath "$ORIGIN;${CMAKE_INSTALL_FULL_LIBDIR}")
  endif()
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES BUILD_RPATH "$ORIGIN;$<TARGET_FILE_DIR:openavx::openavx>" INSTALL_RPATH "${_install_rpath}"
  )
endif()

if(OPENAVX_BUNDLE_RUNTIME)
  if(WIN32)
    add_custom_command(
      TARGET ${CMAKE_PROJECT_NAME}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:openavx::openavx>"
        "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>"
      COMMENT "Bundle OpenAVX DLL into rundir"
      VERBATIM
    )
    install(FILES "$<TARGET_FILE:openavx::openavx>" DESTINATION "${_name}/bin/64bit")
  elseif(APPLE)
    add_custom_command(
      TARGET ${CMAKE_PROJECT_NAME}
      POST_BUILD
      COMMAND
        "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/$<TARGET_BUNDLE_DIR_NAME:${CMAKE_PROJECT_NAME}>/Contents/Frameworks"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:openavx::openavx>"
        "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/$<TARGET_BUNDLE_DIR_NAME:${CMAKE_PROJECT_NAME}>/Contents/Frameworks/$<TARGET_SONAME_FILE_NAME:openavx::openavx>"
      COMMENT "Bundle libopenavx into macOS plugin bundle"
      VERBATIM
    )
    install(
      FILES "$<TARGET_FILE:openavx::openavx>"
      DESTINATION "$<TARGET_BUNDLE_DIR_NAME:${CMAKE_PROJECT_NAME}>/Contents/Frameworks"
      RENAME "$<TARGET_SONAME_FILE_NAME:openavx::openavx>"
    )
  elseif(UNIX)
    add_custom_command(
      TARGET ${CMAKE_PROJECT_NAME}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:openavx::openavx>"
        "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/$<TARGET_FILE_NAME:openavx::openavx>"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:openavx::openavx>"
        "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/$<TARGET_SONAME_FILE_NAME:openavx::openavx>"
      COMMENT "Bundle libopenavx into rundir"
      VERBATIM
    )
    install(
      PROGRAMS "$<TARGET_FILE:openavx::openavx>"
      DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins"
      RENAME "$<TARGET_SONAME_FILE_NAME:openavx::openavx>"
    )
  endif()
endif()
