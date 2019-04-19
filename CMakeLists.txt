# The MIT License (MIT)
#
# Copyright (c) 2015-2017 Simon Ninon <simon.ninon@gmail.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

#
# options
#
option(BUILD_TESTS "whether to build tests for the project" OFF)
option(BUILD_EXAMPLES "whether to build the examples" OFF)
option(USE_CUSTOM_TCP_CLIENT "whether to use a tcp client other than tacopie"
       OFF)

#
# config
#
cmake_minimum_required(VERSION 2.8.7)
set(CMAKE_MACOSX_RPATH 1)
include(${CMAKE_ROOT}/Modules/ExternalProject.cmake)

#
# verbose make
#
# set(CMAKE_VERBOSE_MAKEFILE TRUE)

#
# project
#
set(PROJECT cpp_redis)
project(${PROJECT} CXX)

#
# compilation options
#
if(MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3 /O2 /bigobj")

  # was causing conflics with gtest build
  string(REPLACE "/RTC1"
                 ""
                 CMAKE_CXX_FLAGS_DEBUG
                 ${CMAKE_CXX_FLAGS_DEBUG})

  if("${MSVC_RUNTIME_LIBRARY_CONFIG}" STREQUAL "")
    set(MSVC_RUNTIME_LIBRARY_CONFIG "/MT")
  endif()

  foreach(flag_var
          CMAKE_CXX_FLAGS
          CMAKE_CXX_FLAGS_DEBUG
          CMAKE_CXX_FLAGS_RELEASE)
    if("${MSVC_RUNTIME_LIBRARY_CONFIG}" STREQUAL "/MT")
      string(REPLACE "/MD"
                     "/MT"
                     ${flag_var}
                     "${${flag_var}}")
    elseif("${MSVC_RUNTIME_LIBRARY_CONFIG}" STREQUAL "/MD")
      string(REPLACE "/MT"
                     "/MD"
                     ${flag_var}
                     "${${flag_var}}")
    else()
      string(REPLACE "/MD"
                     "${MSVC_RUNTIME_LIBRARY_CONFIG}"
                     ${flag_var}
                     "${${flag_var}}")
      string(REPLACE "/MT"
                     "${MSVC_RUNTIME_LIBRARY_CONFIG}"
                     ${flag_var}
                     "${${flag_var}}")
    endif()
  endforeach()

  add_definitions(-D_UNICODE)
  add_definitions(-DUNICODE)
  add_definitions(-DWIN32_LEAN_AND_MEAN)
else()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -W -Wall -Wextra -O3")
endif()

#
# variables
#
set(VENDOR_DIR ${PROJECT_SOURCE_DIR}/vendor)
set(VENDOR_INCLUDES ${VENDOR_DIR}/include)
set(VENDOR_LIBRARY_DIR ${VENDOR_DIR}/lib ${VENDOR_DIR}/lib64)

set(CPP_REDIS_INCLUDES ${PROJECT_SOURCE_DIR}/include)

if(NOT USE_CUSTOM_TCP_CLIENT)
  set(VENDOR_INCLUDES ${VENDOR_INCLUDES} ${TACOPIE_INCLUDE_DIR})
endif()

#
# includes
#
include_directories(${CPP_REDIS_INCLUDES} ${VENDOR_INCLUDES})

#
# sources
#
file(GLOB_RECURSE INCLUDES LIST_DIRECTORIES TRUE
     RELATIVE "${PROJECT_SOURCE_DIR}" *.hpp *.ipp)

file(GLOB_RECURSE SOURCES LIST_DIRECTORIES TRUE
     RELATIVE "${PROJECT_SOURCE_DIR}" *.cpp)

list(APPEND SOURCES ${INCLUDES} ${PROJECT_SOURCE_DIR}/include/cpp_redis)

# filter tcp_client if no tacopie
if(USE_CUSTOM_TCP_CLIENT)
  file(GLOB tacopie_cpp "sources/cpp_redis/network/tcp_client.cpp")
  file(GLOB tacopie_h "includes/cpp_redis/network/tcp_client.hpp")
  list(REMOVE_ITEM SOURCES ${tacopie_cpp} ${tacopie_h})
endif()

#
# outputs
#
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_PKGCONFIG_OUTPUT_DIRECTORY
    ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/pkgconfig)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

#
# pkg-config
#
configure_file("cpp_redis.pc.in"
               "${CMAKE_PKGCONFIG_OUTPUT_DIRECTORY}/cpp_redis.pc" @ONLY)

#
# executable
#
add_library(${PROJECT} STATIC ${SOURCES})
set_property(TARGET ${PROJECT} PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(${PROJECT} PRIVATE include)

if(WIN32)
  set_target_properties(${PROJECT}
                        PROPERTIES COMPILE_PDB_NAME
                                   ${PROJECT}
                                   COMPILE_PDB_OUTPUT_DIRECTORY
                                   ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
endif()

if(WIN32)
  target_link_libraries(${PROJECT} ws2_32)
else()
  target_link_libraries(${PROJECT} pthread)
endif()

if(TACOPIE_LIBRARY)
  target_link_libraries(${PROJECT} tacopie)
  target_include_directories(${PROJECT} PUBLIC ${VENDOR_INCLUDES})
else()
  target_link_libraries(${PROJECT} tacopie)
endif()

# __CPP_REDIS_READ_SIZE
if(READ_SIZE)
  set_property(TARGET ${PROJECT}
               APPEND_STRING
               PROPERTY COMPILE_DEFINITIONS
                        " __CPP_REDIS_READ_SIZE=${READ_SIZE}")
endif()

# __CPP_REDIS_LOGGING_ENABLED
if(LOGGING_ENABLED)
  set_property(TARGET ${PROJECT}
               APPEND_STRING
               PROPERTY COMPILE_DEFINITIONS
                        " __CPP_REDIS_LOGGING_ENABLED=${LOGGING_ENABLED}")
endif()

# __CPP_REDIS_USE_CUSTOM_TCP_CLIENT
if(USE_CUSTOM_TCP_CLIENT)
  set_property(
    TARGET ${PROJECT}
    APPEND_STRING
    PROPERTY COMPILE_DEFINITIONS
             " __CPP_REDIS_USE_CUSTOM_TCP_CLIENT=${USE_CUSTOM_TCP_CLIENT}")
endif()

#
# install
#
# ensure lib and bin directories exist
install(CODE "FILE(MAKE_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})")
install(CODE "FILE(MAKE_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})")
# install cpp_redis
install(DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/
        DESTINATION lib
        USE_SOURCE_PERMISSIONS)
install(DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/
        DESTINATION bin
        USE_SOURCE_PERMISSIONS)
install(DIRECTORY ${CPP_REDIS_INCLUDES}/
        DESTINATION include
        USE_SOURCE_PERMISSIONS)

#
# tacopie
#
if(NOT TACOPIE_LIBRARY AND NOT USE_CUSTOM_TCP_CLIENT)
  externalproject_add(tacopiegit
                      GIT_REPOSITORY
                      "https://github.com/Cylix/tacopie.git"
                      CMAKE_ARGS
                      "-DCMAKE_INSTALL_PREFIX=${VENDOR_DIR}"
                      UPDATE_DISCONNECTED
                      OFF)

  set(TACOPIE_INCLUDE_DIR ${VENDOR_INCLUDES})
  set(TACOPIE_LIBRARY_DIR ${VENDOR_LIBRARY_DIR})
  set(TACOPIE_LIBRARY tacopie)
endif()

#
# examples
#
if(BUILD_EXAMPLES)
  add_subdirectory(examples)
  # Reset variable to false to ensure tacopie does no build examples
  set(BUILD_EXAMPLES false)
endif()

#
# tests
#
if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  externalproject_add("googletest"
                      GIT_REPOSITORY
                      "https://github.com/google/googletest.git"
                      CMAKE_ARGS
                      "-DCMAKE_INSTALL_PREFIX=${VENDOR_DIR}"
                      UPDATE_DISCONNECTED
                      ON)
  # Reset variable to false to ensure tacopie does no build tests
  set(BUILD_TESTS false)
endif()
