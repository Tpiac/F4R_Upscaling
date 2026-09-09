# --- Project ---

cmake_minimum_required(VERSION 4.3.0)

set(COMMONLIB_PLUGIN_NAME "F4R_Upscaling" CACHE STRING "Plugin file name (F4R_Upscaling / DLSS / FSR3 / XeSS)")
set(COMMONLIB_PLUGIN_AUTHOR "Tpiac")
set(COMMONLIB_PLUGIN_LICENSE "GPL-3.0-or-later")
set(COMMONLIB_PLUGIN_DESCRIPTION "${COMMONLIB_PLUGIN_NAME}")

if(COMMONLIB_PLUGIN_NAME STREQUAL "DLSS")
	set(F4R_HAS_DLSS 1)
	set(F4R_HAS_FSR3 0)
	set(F4R_HAS_XESS 0)
	set(F4R_DEFAULT_Method 2)
	set(COMMONLIB_PLUGIN_VERSION "1.1.0.0")
elseif(COMMONLIB_PLUGIN_NAME STREQUAL "FSR3")
	set(F4R_HAS_DLSS 0)
	set(F4R_HAS_FSR3 1)
	set(F4R_HAS_XESS 0)
	set(F4R_DEFAULT_Method 1)
	set(COMMONLIB_PLUGIN_VERSION "1.1.0.0")
elseif(COMMONLIB_PLUGIN_NAME STREQUAL "XeSS")
	set(F4R_HAS_DLSS 0)
	set(F4R_HAS_FSR3 0)
	set(F4R_HAS_XESS 1)
	set(F4R_DEFAULT_Method 3)
	set(COMMONLIB_PLUGIN_VERSION "1.1.0.0")
else()
	set(F4R_HAS_DLSS 1)
	set(F4R_HAS_FSR3 1)
	set(F4R_HAS_XESS 1)
	set(F4R_DEFAULT_Method 1)
	set(COMMONLIB_PLUGIN_VERSION "1.1.0.0")
endif()

string(TIMESTAMP COMMONLIB_PLUGIN_COPYRIGHT_YEAR "%Y" UTC)

project(
	"${COMMONLIB_PLUGIN_NAME}"
	VERSION "${COMMONLIB_PLUGIN_VERSION}"
	DESCRIPTION "${COMMONLIB_PLUGIN_DESCRIPTION}"
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)
set(CMAKE_OPTIMIZE_DEPENDENCIES ON)

include(GNUInstallDirs)

# --- Options ---

set(COMMONLIB_RUNTIME_VR OFF)
set(COMMONLIB_RUNTIME_OG ON)
set(COMMONLIB_RUNTIME_NG ON)
set(COMMONLIB_RUNTIME_AE ON)

set(COMMONLIB_OPTION_FMT ON)
set(COMMONLIB_OPTION_XBYAK OFF)
set(COMMONLIB_OPTION_INI OFF)
set(COMMONLIB_OPTION_TOML OFF)
set(COMMONLIB_OPTION_JSON OFF)

# --- Add Sources ---

set(
	PCH_SOURCE
	"${CMAKE_CURRENT_SOURCE_DIR}/src/PCH.hpp"
)

set(
	COMMONLIB_ROOT_DIR
	"${CMAKE_CURRENT_SOURCE_DIR}/lib/CommonLibF4"
)

include("${COMMONLIB_ROOT_DIR}/cmake/GenerateXsePluginInfo.cmake")
include("${COMMONLIB_ROOT_DIR}/cmake/SetXseOutputPath.cmake")
include("${COMMONLIB_ROOT_DIR}/cmake/CopyXseOutput.cmake")

generate_xse_plugin_info("${COMMONLIB_ROOT_DIR}")

file(
	GLOB_RECURSE
		SOURCES
	CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/src/*"
)

list(
	REMOVE_ITEM
		SOURCES
		"${PCH_SOURCE}"
)

source_group(
	TREE
		"${CMAKE_CURRENT_SOURCE_DIR}"
	FILES
		${SOURCES}
)

source_group(
	TREE
		"${CMAKE_CURRENT_BINARY_DIR}/include"
	FILES
		"${XSE_PLUGIN_INFO_CPP_TARGET}"
)

# --- Include ---

if(NOT TARGET CommonLibF4)
	add_subdirectory("${COMMONLIB_ROOT_DIR}" CommonLibF4)
endif()

add_library(
	"${PROJECT_NAME}"
	SHARED
		${SOURCES}
		"${XSE_PLUGIN_INFO_CPP_TARGET}"
		"${WIN32_VERSION_TARGET}"
)

target_include_directories(
	"${PROJECT_NAME}"
	PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
		"${CMAKE_CURRENT_SOURCE_DIR}/include"
		"${CMAKE_CURRENT_SOURCE_DIR}/extern/Streamline/include"
		"${CMAKE_CURRENT_SOURCE_DIR}/extern/FFX-SDK-DX11/include"
		"${CMAKE_CURRENT_SOURCE_DIR}/extern/xess"
		"${CMAKE_CURRENT_SOURCE_DIR}/lib/detours"
		"${CMAKE_CURRENT_BINARY_DIR}/include"
)

target_precompile_headers(
	"${PROJECT_NAME}"
	PRIVATE
		"${PCH_SOURCE}"
)

set_xse_output_path("${PROJECT_NAME}")

# --- Compile ---

target_compile_definitions(
	"${PROJECT_NAME}"
	PRIVATE
		F4R_MODULE_NAME="${COMMONLIB_PLUGIN_NAME}"
		F4R_HAS_DLSS=${F4R_HAS_DLSS}
		F4R_HAS_FSR3=${F4R_HAS_FSR3}
		F4R_HAS_XESS=${F4R_HAS_XESS}
		F4R_DEFAULT_Method=${F4R_DEFAULT_Method}
)

# --- Link ---

# detours.lib is shipped without its PDB; silence LNK4099
target_link_options("${PROJECT_NAME}" PRIVATE "/IGNORE:4099")

target_link_libraries(
	"${PROJECT_NAME}"
	PRIVATE
		CommonLibF4::CommonLibF4
		debug "${CMAKE_CURRENT_SOURCE_DIR}/lib/detours/Debug/detours.lib"
		optimized "${CMAKE_CURRENT_SOURCE_DIR}/lib/detours/Release/detours.lib"
		d3d11
		d3d12
		dxgi
)

if(F4R_HAS_FSR3)
	target_link_libraries(
		"${PROJECT_NAME}"
		PRIVATE
			"${CMAKE_CURRENT_SOURCE_DIR}/extern/FFX-SDK-DX11/lib/ffx_backend_dx11_x64.lib"
			"${CMAKE_CURRENT_SOURCE_DIR}/extern/FFX-SDK-DX11/lib/ffx_fsr3upscaler_x64.lib"
			"${CMAKE_CURRENT_SOURCE_DIR}/extern/FFX-SDK-DX11/lib/ffx_fsr3_x64.lib"
			"${CMAKE_CURRENT_SOURCE_DIR}/extern/FFX-SDK-DX11/lib/ffx_opticalflow_x64.lib"
			"${CMAKE_CURRENT_SOURCE_DIR}/extern/FFX-SDK-DX11/lib/ffx_frameinterpolation_x64.lib"
	)
endif()

# --- Copy Output ---

# LTO disabled for the plugin DLL to prevent breakage of detour trampolines
set_target_properties("${PROJECT_NAME}" PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

copy_xse_copy_output("${PROJECT_NAME}")
