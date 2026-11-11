cmake_minimum_required(VERSION 3.21)

project(restir_gi LANGUAGES C CXX)

include(FetchContent)

if(EMSCRIPTEN)
	# shaderc cannot run as WebAssembly, so the web build uses one built by a
	# native build of this project (build/_deps/bgfx.cmake-build/cmake/bgfx/shaderc).
	set(RESTIR_HOST_SHADERC "" CACHE FILEPATH "Native shaderc used to compile the shaders for the web build")
	if(NOT EXISTS "${RESTIR_HOST_SHADERC}")
		message(FATAL_ERROR "Set RESTIR_HOST_SHADERC to a native shaderc executable, e.g. one built by a native build of this project.")
	endif()
	add_executable(bgfx::shaderc IMPORTED)
	set_target_properties(bgfx::shaderc PROPERTIES IMPORTED_LOCATION "${RESTIR_HOST_SHADERC}")

	set(BGFX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
	set(BGFX_CONFIG_RENDERER_WEBGPU ON CACHE BOOL "" FORCE)
	set(BGFX_INSTALL OFF CACHE BOOL "" FORCE)
else()
	set(BGFX_BUILD_TOOLS ON CACHE BOOL "" FORCE)
	set(BGFX_BUILD_TOOLS_SHADER ON CACHE BOOL "" FORCE)
endif()
set(BGFX_BUILD_TOOLS_BIN2C OFF CACHE BOOL "" FORCE)
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

if(EMSCRIPTEN)
	# bgfx leaves its WebGPU renderer off on Emscripten unless asked; defining
	# it also turns the other renderers off, so the web build is WebGPU only.
	target_compile_definitions(bgfx PUBLIC BGFX_CONFIG_RENDERER_WEBGPU=1)
	set(RESTIR_SHADER_PROFILES PROFILES wgsl)
	set(RESTIR_COMPUTE_PROFILES PROFILES wgsl)
else()
	# The world-space ReSTIR shaders use compute and storage buffers, which
	# GLSL ES 3.0 (the 300_es profile) does not have.
	set(RESTIR_COMPUTE_PROFILES PROFILES spirv 430)
	if(WIN32)
		list(APPEND RESTIR_COMPUTE_PROFILES s_5_0 s_6_0)
	elseif(APPLE)
		list(APPEND RESTIR_COMPUTE_PROFILES metal)
	endif()
	if(BGFX_CONFIG_RENDERER_WEBGPU)
		list(APPEND RESTIR_COMPUTE_PROFILES wgsl)
	endif()
endif()

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
	${RESTIR_SHADER_PROFILES}
)

bgfx_compile_shaders(
	TYPE FRAGMENT
	SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_gbuffer.sc"
		"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_composite.sc"
		"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_shadow.sc"
		"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_svgf_variance.sc"
		"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_svgf_atrous.sc"
	VARYING_DEF "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
	OUTPUT_DIR "${BGFX_SHADER_OUTPUT_DIR}"
	OUT_FILES_VAR BGFX_FRAGMENT_SHADERS
	INCLUDE_DIRS "${BGFX_SHADER_COMMON_INCLUDE_PATH}"
	${RESTIR_SHADER_PROFILES}
)

bgfx_compile_shaders(
	TYPE FRAGMENT
	SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_restir_temporal.sc"
		"${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_restir_spatial.sc"
	VARYING_DEF "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
	OUTPUT_DIR "${BGFX_SHADER_OUTPUT_DIR}"
	OUT_FILES_VAR BGFX_RESTIR_SHADERS
	INCLUDE_DIRS "${BGFX_SHADER_COMMON_INCLUDE_PATH}"
	${RESTIR_COMPUTE_PROFILES}
)

bgfx_compile_shaders(
	TYPE COMPUTE
	SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cs_grid_clear.sc"
		"${CMAKE_CURRENT_SOURCE_DIR}/shaders/cs_grid_insert.sc"
	VARYING_DEF "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
	OUTPUT_DIR "${BGFX_SHADER_OUTPUT_DIR}"
	OUT_FILES_VAR BGFX_COMPUTE_SHADERS
	INCLUDE_DIRS "${BGFX_SHADER_COMMON_INCLUDE_PATH}"
	${RESTIR_COMPUTE_PROFILES}
)

bgfx_compile_shaders(
	TYPE VERTEX
	SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_fullscreen.sc"
	VARYING_DEF "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
	OUTPUT_DIR "${BGFX_SHADER_OUTPUT_DIR}"
	OUT_FILES_VAR BGFX_FULLSCREEN_SHADERS
	INCLUDE_DIRS "${BGFX_SHADER_COMMON_INCLUDE_PATH}"
	${RESTIR_SHADER_PROFILES}
)

set(BGFX_RUNTIME_SHADERS)
foreach(shader IN LISTS BGFX_VERTEX_SHADERS BGFX_FRAGMENT_SHADERS BGFX_FULLSCREEN_SHADERS
		BGFX_RESTIR_SHADERS BGFX_COMPUTE_SHADERS)
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
	shaders/fs_restir_temporal.sc
	shaders/fs_restir_spatial.sc
	shaders/restir.sh
	shaders/hashgrid.sh
	shaders/cs_grid_clear.sc
	shaders/cs_grid_insert.sc
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

if(EMSCRIPTEN)
	# The shaders and assets are packaged into restir_gi.data, which the page
	# downloads at start-up. Asyncify lets bgfx wait on the browser's
	# asynchronous WebGPU adapter and device requests.
	set_target_properties(restir_gi PROPERTIES SUFFIX ".html")
	target_link_options(restir_gi PRIVATE
		"-sASYNCIFY=1"
		"-sASYNCIFY_STACK_SIZE=1048576"
		"-sALLOW_MEMORY_GROWTH=1"
		"-sENVIRONMENT=web"
		"--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
		"--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/web/webgpu_workarounds.js"
		"--preload-file=${BGFX_SHADER_OUTPUT_DIR}@shaders"
		"--exclude-file=*.sc.bin"
		"--preload-file=${CMAKE_CURRENT_SOURCE_DIR}/assets@assets"
	)
	set_property(TARGET restir_gi APPEND PROPERTY LINK_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
		"${CMAKE_CURRENT_SOURCE_DIR}/web/webgpu_workarounds.js"
		${BGFX_RUNTIME_SHADERS}
	)
else()
	# Copy the shaders and models beside the executable on every build, not
	# only when it relinks, so that a changed shader or a model fetched later
	# (cmake/fetch_sponza.cmake) shows up after the next build.
	if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.26)
		set(RESTIR_COPY_DIRECTORY copy_directory_if_different)
	else()
		set(RESTIR_COPY_DIRECTORY copy_directory)
	endif()
	add_custom_target(restir_gi_runtime_files ALL
		COMMAND ${CMAKE_COMMAND} -E ${RESTIR_COPY_DIRECTORY}
			"${BGFX_SHADER_OUTPUT_DIR}"
			"$<TARGET_FILE_DIR:restir_gi>/shaders"
		COMMAND ${CMAKE_COMMAND} -E ${RESTIR_COPY_DIRECTORY}
			"${CMAKE_CURRENT_SOURCE_DIR}/assets"
			"$<TARGET_FILE_DIR:restir_gi>/assets"
		DEPENDS ${BGFX_RUNTIME_SHADERS}
		COMMENT "Copying shaders and assets beside restir_gi"
		VERBATIM
	)
	# Building restir_gi alone (as the VS Code task does) copies them too.
	add_dependencies(restir_gi restir_gi_runtime_files)
	set_target_properties(restir_gi_runtime_files PROPERTIES FOLDER "restir")
endif()

set_target_properties(restir_gi PROPERTIES
	VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:restir_gi>"
	FOLDER "restir"
)
