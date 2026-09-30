cmake_minimum_required(VERSION 3.20)
project(piggle VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# C11 ABI implemented in C++11 over the shared runtime foundation.
option(BUILD_SHARED_LIBS "Build Piggle as a shared library" OFF)
include(cmake/runtime-foundation.cmake)

# Apply these settings only to Piggle targets, after configuring zlib-ng.
if(MSVC)
	add_compile_definitions(_CRT_SECURE_NO_WARNINGS
		_CRT_NONSTDC_NO_WARNINGS)
	add_compile_options(/WX)
	add_link_options(/WX)
else()
	add_compile_options(-Werror)
	add_link_options(-Werror)
	if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR MINGW)
		add_link_options(LINKER:--fatal-warnings)
	elseif(APPLE)
		add_link_options(LINKER:-fatal_warnings)
	endif()
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT piggle_ipo_supported
	OUTPUT piggle_ipo_error LANGUAGES C CXX)
if(NOT piggle_ipo_supported)
	message(STATUS "Release IPO unavailable: ${piggle_ipo_error}")
endif()

if((CMAKE_SYSTEM_NAME STREQUAL "Linux" OR MINGW) AND NOT MSVC)
	add_compile_options($<$<CONFIG:Release>:-ffunction-sections>
		$<$<CONFIG:Release>:-fdata-sections>)
	add_link_options($<$<CONFIG:Release>:LINKER:--gc-sections>)
endif()
add_library(piggle
	src/api.cpp
	src/archive_builder.cpp
	src/change.cpp
	src/file.cpp
	src/io.cpp
	src/native_path.cpp
	src/source.cpp
	src/tree.cpp
	src/types.cpp
)
if(piggle_ipo_supported)
	set_property(TARGET piggle PROPERTY
		INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
	if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
		NOT BUILD_SHARED_LIBS AND NOT MSVC)
		list(GET CMAKE_CXX_COMPILE_OPTIONS_IPO 0 piggle_ipo_link_flag)
		target_link_options(piggle INTERFACE
			"$<$<CONFIG:Release>:${piggle_ipo_link_flag}>")
	endif()
endif()
add_library(piggle::piggle ALIAS piggle)
piggle_configure_runtime(piggle)
target_include_directories(piggle PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
	option(PIGGLE_BUILD_CLI "Build the piggle archive command" ON)
else()
	option(PIGGLE_BUILD_CLI "Build the piggle archive command" OFF)
endif()
if(PIGGLE_BUILD_CLI)
	add_subdirectory(cli)
endif()
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
	option(BUILD_TESTING "Build Piggle contract and infrastructure tests" ON)
else()
	option(BUILD_TESTING "Build Piggle contract and infrastructure tests" OFF)
endif()
include(CTest)
if(BUILD_TESTING)
	add_subdirectory(tests)
endif()

install(TARGETS piggle EXPORT piggleTargets
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
set(piggle_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/piggle")
configure_package_config_file(cmake/piggleConfig.cmake.in
	"${CMAKE_CURRENT_BINARY_DIR}/piggleConfig.cmake"
	INSTALL_DESTINATION "${piggle_package_dir}"
)
write_basic_package_version_file(
	"${CMAKE_CURRENT_BINARY_DIR}/piggleConfigVersion.cmake"
	VERSION ${PROJECT_VERSION}
	COMPATIBILITY SameMinorVersion
)
install(EXPORT piggleTargets NAMESPACE piggle::
	DESTINATION "${piggle_package_dir}"
)
install(FILES
	"${CMAKE_CURRENT_BINARY_DIR}/piggleConfig.cmake"
	"${CMAKE_CURRENT_BINARY_DIR}/piggleConfigVersion.cmake"
	DESTINATION "${piggle_package_dir}"
)
