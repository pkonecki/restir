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
	set(RESTIR_SHADER_PROFILES wgsl)
	set(RESTIR_COMPUTE_PROFILES wgsl)
else()
	# Empty: bgfx's default profiles for the platform.
	set(RESTIR_SHADER_PROFILES)
	# The world-space ReSTIR shaders use compute and storage buffers, which
	# GLSL ES 3.0 (the 300_es profile) does not have.
	set(RESTIR_COMPUTE_PROFILES spirv 430)
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

# Compiles shaders/<name>.sc for each name, for the given profiles (bgfx's
# defaults if none), and appends the outputs to RESTIR_SHADER_BINARIES.
function(restir_compile_shaders type)
	cmake_parse_arguments(ARG "" "" "SHADERS;PROFILES" ${ARGN})
	list(TRANSFORM ARG_SHADERS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/shaders/")
	list(TRANSFORM ARG_SHADERS APPEND ".sc")
	if(ARG_PROFILES)
		set(profiles PROFILES ${ARG_PROFILES})
	endif()
	bgfx_compile_shaders(
		TYPE ${type}
		SHADERS ${ARG_SHADERS}
		VARYING_DEF "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
		OUTPUT_DIR "${BGFX_SHADER_OUTPUT_DIR}"
		OUT_FILES_VAR outputs
		INCLUDE_DIRS "${BGFX_SHADER_COMMON_INCLUDE_PATH}"
		${profiles}
	)
	set(RESTIR_SHADER_BINARIES ${RESTIR_SHADER_BINARIES} ${outputs} PARENT_SCOPE)
endfunction()

set(RESTIR_SHADER_BINARIES)
restir_compile_shaders(VERTEX
	SHADERS vs_scene vs_shadow vs_fullscreen
	PROFILES ${RESTIR_SHADER_PROFILES})
restir_compile_shaders(FRAGMENT
	SHADERS fs_gbuffer fs_composite fs_shadow fs_svgf_variance fs_svgf_atrous
	PROFILES ${RESTIR_SHADER_PROFILES})
# Fragment shaders that read storage buffers need the compute profiles too.
restir_compile_shaders(FRAGMENT
	SHADERS fs_restir_temporal fs_restir_spatial fs_tonemap fs_debug
	PROFILES ${RESTIR_COMPUTE_PROFILES})
restir_compile_shaders(COMPUTE
	SHADERS cs_grid_clear cs_grid_insert cs_exposure
	PROFILES ${RESTIR_COMPUTE_PROFILES})

# The runtime looks for <name>.bin, not <name>.sc.bin.
set(BGFX_RUNTIME_SHADERS)
foreach(shader IN LISTS RESTIR_SHADER_BINARIES)
	string(REGEX REPLACE "\\.sc\\.bin$" ".bin" runtime_shader "${shader}")
	add_custom_command(
		OUTPUT "${runtime_shader}"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${shader}" "${runtime_shader}"
		DEPENDS "${shader}"
		VERBATIM
	)
	list(APPEND BGFX_RUNTIME_SHADERS "${runtime_shader}")
endforeach()

# Shader sources are listed so that IDEs show them.
file(GLOB RESTIR_SHADER_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.sc"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.sh")

add_executable(restir_gi
	src/main.cpp
	src/bookmarks.cpp
	src/bookmarks.h
	src/bvh.cpp
	src/bvh.h
	src/camera.cpp
	src/camera.h
	src/capture.cpp
	src/capture.h
	src/renderer.cpp
	src/renderer.h
	src/scene.cpp
	src/scene.h
	src/settings.h
	src/ui.cpp
	src/ui.h
	${RESTIR_SHADER_SOURCES}
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
