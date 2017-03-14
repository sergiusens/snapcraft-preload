# -*- Mode: CMake; indent-tabs-mode: nil; tab-width: 4 -*-
#
# Copyright (C) 2015-2016 Canonical, Ltd.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 3.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.2)

set(LIBNAME "libsnapcraft-preload")
add_library(snapcraft-preload SHARED preload.cpp)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14")

set_target_properties(snapcraft-preload PROPERTIES
                      COMPILE_FLAGS "-DSNAPCRAFT_LIBNAME=\\\"${LIBNAME}.so\\\"")
target_link_libraries(snapcraft-preload -ldl)

execute_process(COMMAND uname -m COMMAND tr -d '\n' OUTPUT_VARIABLE ARCHITECTURE)
if(${ARCHITECTURE} STREQUAL "x86_64")
    add_library(snapcraft-preload32 SHARED preload.cpp)
    set_target_properties(snapcraft-preload32 PROPERTIES
                          COMPILE_FLAGS "-DSNAPCRAFT_LIBNAME=\\\"${LIBNAME}32.so\\\" -m32")
    target_link_libraries(snapcraft-preload32 -ldl -m32)
endif()

install(TARGETS snapcraft-preload LIBRARY DESTINATION lib)
install(PROGRAMS snapcraft-preload DESTINATION bin)
