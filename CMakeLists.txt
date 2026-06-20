# Copyright © 2026 SculkCatalystMC. All rights reserved.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, version 3 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

cmake_minimum_required(VERSION 3.20)

project(ProxyPass LANGUAGES CXX)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include(FetchContent)
find_package(OpenSSL REQUIRED)
set(PROTOCOL_ENABLE_FORMATTING ON CACHE BOOL "Enable formatting support in Protocol")
FetchContent_Declare(
	Protocol
	GIT_REPOSITORY https://github.com/SculkCatalystMC/Protocol.git
	GIT_TAG main
)
FetchContent_Declare(
	reflection
	GIT_REPOSITORY https://github.com/SculkCatalystMC/Reflection.git
	GIT_TAG main
)
FetchContent_Declare(
	parallel_hashmap
	GIT_REPOSITORY https://github.com/greg7mdp/parallel-hashmap.git
	GIT_TAG v2.0.0
)
FetchContent_MakeAvailable(Protocol reflection parallel_hashmap)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(GLOB_RECURSE PROJECT_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC
	$<BUILD_INTERFACE:${parallel_hashmap_SOURCE_DIR}>
)

target_link_libraries(${PROJECT_NAME} PRIVATE
	Protocol::Protocol
	reflection::reflection
	jsonc::jsonc
	zlibstatic
)

target_include_directories(${PROJECT_NAME}
	PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_23)

if(MSVC)
	target_sources(${PROJECT_NAME} PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src/Version.rc"
	)

	string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
	string(REPLACE "/Ob2" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
	string(REPLACE "/Ob2" "" CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL}")

	target_compile_options(${PROJECT_NAME} PRIVATE
		/W4
		/utf-8
		/GR-
		/Zc:preprocessor
		/permissive-
		/EHs-c-
		$<$<CONFIG:Release>:/O2>
		$<$<CONFIG:Release>:/Ob3>
		$<$<CONFIG:MinSizeRel>:/O2>
		$<$<CONFIG:MinSizeRel>:/Ob3>
	)
	target_compile_definitions(${PROJECT_NAME} PRIVATE
		UNICODE
		NOMINMAX
		_HAS_EXCEPTIONS=0
	)
else()
	target_compile_options(${PROJECT_NAME} PRIVATE
		-Wall
		-Wextra
		-Wpedantic
		-fno-rtti
		-fno-exceptions
		-static
		-static-libgcc
		-static-libstdc++
		$<$<CONFIG:Release>:-O3>
		$<$<CONFIG:RelWithDebInfo>:-O3>
	)
	target_link_options(${PROJECT_NAME} PRIVATE
		-Wl,--no-undefined
		$<$<NOT:$<CONFIG:Debug>>:-Wl,--exclude-libs,ALL>
		$<$<NOT:$<CONFIG:Debug>>:-s>
	)
endif()

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${PROJECT_SOURCES})

set(PROJECT_BIN_DIR "${CMAKE_SOURCE_DIR}/bin")
set(PROJECT_LIB_DIR "${CMAKE_BINARY_DIR}/lib")

set_target_properties(${PROJECT_NAME} PROPERTIES
	OUTPUT_NAME "ProxyPass"
	RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BIN_DIR}"
	LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BIN_DIR}"
	ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_LIB_DIR}"
)
