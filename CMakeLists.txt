cmake_minimum_required(VERSION 3.21)

project(restir_gi LANGUAGES C CXX)

include(FetchContent)

set(BGFX_BUILD_TOOLS ON CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_BIN2C OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_SHADER ON CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_GEOMETRY OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_TEXTURE OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_EXAMPLE_COMMON ON CACHE BOOL "" FORCE)
set(BGFX_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
	bgfx.cmake
	GIT_REPOSITORY https://github.com/bkaradzic/bgfx.cmake.git
	GIT_TAG a046ef085427ff7dd5a499d7f48e06f5d1bb049d
	GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(bgfx.cmake)

set(FASTGLTF_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_DOCS OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(FASTGLTF_COMPILE_AS_CPP20 ON CACHE BOOL "" FORCE)

FetchContent_Declare(
	fastgltf
	GIT_REPOSITORY https://github.com/spnda/fastgltf.git
	GIT_TAG v0.9.1
	GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(fastgltf)

set(BGFX_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(BGFX_SHADER_INCLUDE_PATH "${bgfx.cmake_SOURCE_DIR}/bgfx/src")
set(BGFX_SHADER_COMMON_INCLUDE_PATH "${bgfx.cmake_SOURCE_DIR}/bgfx/examples/common")

bgfx_compile_shaders(
	TYPE VERTEX
	SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_scene.sc"
		"${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_shadow.sc"
	VARYING_DEF "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
	OUTPUT_DIR "${BGFX_SHADER_OUTPUT_DIR}"
	OUT_FILES_VAR BGFX_VERTEX_SHADERS
	INCLUDE_DIRS "${BGFX_SHADER_COMMON_INCLUDE_PATH}"
)

bgfx_compile_shaders(
	TYPE FRAGMENT
	SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_gbuffer.sc"
		"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_restir_gi.sc"
		"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_composite.sc"
		"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_shadow.sc"
		"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_svgf_variance.sc"
		"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_svgf_atrous.sc"
	VARYING_DEF "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
	OUTPUT_DIR "${BGFX_SHADER_OUTPUT_DIR}"
	OUT_FILES_VAR BGFX_FRAGMENT_SHADERS
	INCLUDE_DIRS "${BGFX_SHADER_COMMON_INCLUDE_PATH}"
)

bgfx_compile_shaders(
	TYPE VERTEX
	SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_fullscreen.sc"
	VARYING_DEF "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
	OUTPUT_DIR "${BGFX_SHADER_OUTPUT_DIR}"
	OUT_FILES_VAR BGFX_FULLSCREEN_SHADERS
	INCLUDE_DIRS "${BGFX_SHADER_COMMON_INCLUDE_PATH}"
)

set(BGFX_RUNTIME_SHADERS)
foreach(shader IN LISTS BGFX_VERTEX_SHADERS BGFX_FRAGMENT_SHADERS BGFX_FULLSCREEN_SHADERS)
	string(REGEX REPLACE "\\.sc\\.bin$" ".bin" runtime_shader "${shader}")
	add_custom_command(
		OUTPUT "${runtime_shader}"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${shader}" "${runtime_shader}"
		DEPENDS "${shader}"
		VERBATIM
	)
	list(APPEND BGFX_RUNTIME_SHADERS "${runtime_shader}")
endforeach()

add_executable(restir_gi
	src/main.cpp
	shaders/vs_scene.sc
	shaders/vs_shadow.sc
	shaders/fs_shadow.sc
	shaders/shadow.sh
	shaders/fs_svgf_variance.sc
	shaders/fs_svgf_atrous.sc
	shaders/svgf.sh
	shaders/fs_gbuffer.sc
	shaders/fs_restir_gi.sc
	shaders/fs_composite.sc
	shaders/vs_fullscreen.sc
	shaders/varying.def.sc
	${BGFX_RUNTIME_SHADERS}
)

target_compile_features(restir_gi PRIVATE cxx_std_20)
target_compile_definitions(restir_gi PRIVATE
	ENTRY_CONFIG_IMPLEMENT_MAIN=1
	_CRT_SECURE_NO_WARNINGS
	__STDC_FORMAT_MACROS
)
target_link_libraries(restir_gi PRIVATE example-common fastgltf::fastgltf)

add_custom_command(
	TARGET restir_gi POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${BGFX_SHADER_OUTPUT_DIR}"
		"$<TARGET_FILE_DIR:restir_gi>/shaders"
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${CMAKE_CURRENT_SOURCE_DIR}/assets"
		"$<TARGET_FILE_DIR:restir_gi>/assets"
	VERBATIM
)

set_target_properties(restir_gi PROPERTIES
	VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:restir_gi>"
	FOLDER "restir"
)
