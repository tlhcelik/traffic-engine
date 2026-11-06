cmake_minimum_required(VERSION 3.16)
project(traffic_engine LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# The ui files are in ../ui
set(CMAKE_AUTOUIC_SEARCH_PATHS ${CMAKE_SOURCE_DIR}/../ui)

# ---------------------------------------------------------------------------
# Folders. build/setup_cots.ps1 collects the third-party components in ../cots.
# ---------------------------------------------------------------------------
get_filename_component(TE_ROOT "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
set(TE_COTS "${TE_ROOT}/cots")
set(TE_BIN  "${TE_ROOT}/bin")

foreach(_c qt5 boost xerces-c)
	if(NOT EXISTS "${TE_COTS}/${_c}")
		message(FATAL_ERROR "cots/${_c} is missing. Run build/setup_cots.bat first.")
	endif()
endforeach()

list(PREPEND CMAKE_PREFIX_PATH "${TE_COTS}/qt5" "${TE_COTS}/xerces-c")

# Qt 5
find_package(Qt5 5.12 REQUIRED COMPONENTS Core Gui Widgets OpenGL Network Svg Concurrent)
find_package(OpenGL REQUIRED)

# Xerces-C: all XML files (configuration and .te workspace files).
# The Xerces-C install has a CMake package file. If it is not there, use FindXercesC.
find_package(XercesC CONFIG QUIET PATHS "${TE_COTS}/xerces-c" NO_DEFAULT_PATH)
if(NOT XercesC_FOUND)
	set(XercesC_ROOT "${TE_COTS}/xerces-c")
	find_package(XercesC MODULE REQUIRED)
endif()

# Xerces-C DLLs (Debug: ...D.dll). If there is no Debug DLL, the Release DLL is used.
file(GLOB _xerces_dlls "${TE_COTS}/xerces-c/bin/xerces-c_3_*.dll")
set(TE_XERCES_DLL_DEBUG "")
set(TE_XERCES_DLL_RELEASE "")
foreach(_d ${_xerces_dlls})
	if(_d MATCHES "D\\.dll$")
		set(TE_XERCES_DLL_DEBUG "${_d}")
	else()
		set(TE_XERCES_DLL_RELEASE "${_d}")
	endif()
endforeach()
if(NOT TE_XERCES_DLL_DEBUG)
	set(TE_XERCES_DLL_DEBUG "${TE_XERCES_DLL_RELEASE}")
endif()

# Boost: headers only (geometry R-tree, graph).
set(TE_BOOST_INCLUDE "${TE_COTS}/boost")
if(NOT EXISTS "${TE_BOOST_INCLUDE}/boost/version.hpp")
	message(FATAL_ERROR "cots/boost/boost/version.hpp is missing. Run build/setup_cots.bat.")
endif()

set(SOURCES
	../src/main.cpp
	../src/app_paths.cpp
	../src/app_paths.h
	../src/xml_doc.cpp
	../src/xml_doc.h
	../src/config_common.cpp
	../src/config_common.h
	../src/logger.cpp
	../src/logger.h
	../src/log_compressor.cpp
	../src/log_compressor.h
	../src/crash_handler.cpp
	../src/crash_handler.h
	../src/debug_tools.cpp
	../src/debug_tools.h
	../src/app_theme.cpp
	../src/app_theme.h
	../src/i18n.cpp
	../src/i18n.h
	../src/geom.cpp
	../src/geom.h
	../src/geo.cpp
	../src/geo.h
	../src/http_client.cpp
	../src/http_client.h
	../src/world_model.cpp
	../src/world_model.h
	../src/sign_catalog.cpp
	../src/sign_catalog.h
	../src/sign_painter.cpp
	../src/road_network.cpp
	../src/road_network.h
	../src/route_planner.cpp
	../src/route_planner.h
	../src/vehicle_types.cpp
	../src/vehicle_types.h
	../src/stats_collector.cpp
	../src/stats_collector.h
	../src/simulation.cpp
	../src/simulation.h
	../src/sim_pedestrians.cpp
	../src/sim_checks.cpp
	../src/sim_congestion.cpp
	../src/congestion_dock.cpp
	../src/congestion_dock.h
	../src/sim_test.cpp
	../src/sim_test.h
	../src/splash_screen.cpp
	../src/splash_screen.h
	../src/world_properties.cpp
	../src/world_properties.h
	../src/te_file.cpp
	../src/te_file.h
	../src/workspace.cpp
	../src/workspace.h
	../src/world_layout.cpp
	../src/world_layout.h
	../src/mesh_builder.cpp
	../src/mesh_builder.h
	../src/world_renderer.cpp
	../src/world_renderer.h
	../src/world_view.cpp
	../src/world_view.h
	../src/icon_factory.cpp
	../src/icon_factory.h
	../src/property_panel.cpp
	../src/property_panel.h
	../src/sign_palette.cpp
	../src/sign_palette.h
	../src/chart_widget.cpp
	../src/chart_widget.h
	../src/chart_dock.cpp
	../src/chart_dock.h
	../src/traffic_light_dialog.cpp
	../src/traffic_light_dialog.h
	../src/settings_dialog.cpp
	../src/settings_dialog.h
	../src/traffic_density_widget.cpp
	../src/traffic_density_widget.h
	../src/json_export.cpp
	../src/json_export.h
	../src/api_server.cpp
	../src/api_server.h
	../src/osm_import.cpp
	../src/osm_import.h
	../src/osm_import_dialog.cpp
	../src/osm_import_dialog.h
	../src/osm_area_map.cpp
	../src/osm_area_map.h
	../src/tile_cache.cpp
	../src/tile_cache.h
	../src/demo_world.cpp
	../src/demo_world.h
	../src/main_window.cpp
	../src/main_window.h
	../ui/main_window.ui
	../resources/resources.qrc
)

# Windows program icon
set(TE_WIN_RC "")
if(WIN32)
	set(TE_WIN_RC ${CMAKE_SOURCE_DIR}/../resources/traffic_engine.rc)
endif()

add_executable(traffic_engine ${SOURCES} ${TE_WIN_RC})

# Debug: console window for the log lines. Release: GUI program (no console).
# The debug mode (--debug) of a Release build opens a console when necessary.
if(WIN32)
	set_target_properties(traffic_engine PROPERTIES
		WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
		VS_DEBUGGER_COMMAND "${TE_BIN}/$<CONFIG>/traffic_engine.exe"
		VS_DEBUGGER_WORKING_DIRECTORY "${TE_BIN}/$<CONFIG>")
	set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT traffic_engine)
	target_link_libraries(traffic_engine PRIVATE $<$<NOT:$<CONFIG:Debug>>:Qt5::WinMain>)
endif()

# Program version: one source (project VERSION). The code uses TE_VERSION.
target_compile_definitions(traffic_engine PRIVATE
	TE_VERSION="${PROJECT_VERSION}"
	TE_BUILD_CONFIG="$<CONFIG>"
	NOMINMAX
	_USE_MATH_DEFINES
	BOOST_ALL_NO_LIB)

if(MSVC)
	target_compile_options(traffic_engine PRIVATE /W3 /utf-8 /MP /bigobj)
	# Release has debug information (PDB) too: the crash dumps need it.
	target_compile_options(traffic_engine PRIVATE $<$<CONFIG:Release>:/Zi>)
	target_link_options(traffic_engine PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endif()

target_include_directories(traffic_engine PRIVATE ${CMAKE_SOURCE_DIR}/../src)
target_include_directories(traffic_engine SYSTEM PRIVATE ${TE_BOOST_INCLUDE})
target_link_libraries(traffic_engine PRIVATE
	Qt5::Widgets Qt5::OpenGL Qt5::Network Qt5::Svg Qt5::Concurrent
	OpenGL::GL XercesC::XercesC)
# Crash handler: MiniDumpWriteDump and the stack trace (DbgHelp)
# Network (OpenStreetMap, map tiles): WinHTTP (TLS of Windows, no OpenSSL DLLs)
if(WIN32)
	target_link_libraries(traffic_engine PRIVATE dbghelp psapi winhttp)
endif()

# The deploy script reads the version from this file.
file(WRITE "${CMAKE_BINARY_DIR}/te_version.txt" "${PROJECT_VERSION}")

# After each build: symbolic links in bin/<config> and windeployqt.
add_custom_command(TARGET traffic_engine POST_BUILD
	COMMAND ${CMAKE_COMMAND}
		"-DTE_CONFIG=$<CONFIG>"
		"-DTE_EXE=$<TARGET_FILE:traffic_engine>"
		"-DTE_PDB=$<TARGET_PDB_FILE:traffic_engine>"
		"-DTE_BIN_DIR=${TE_BIN}/$<CONFIG>"
		"-DTE_ROOT=${TE_ROOT}"
		"-DTE_COTS=${TE_COTS}"
		"-DTE_XERCES_DLL=$<IF:$<CONFIG:Debug>,${TE_XERCES_DLL_DEBUG},${TE_XERCES_DLL_RELEASE}>"
		-P "${CMAKE_SOURCE_DIR}/cmake/link_to_bin.cmake"
	COMMENT "bin/$<CONFIG>: symbolic links and windeployqt"
	VERBATIM)
