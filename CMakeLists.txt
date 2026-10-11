cmake_minimum_required(VERSION 3.24)
project(piggle VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)
include(CheckIPOSupported)
check_ipo_supported(RESULT piggle_ipo_supported
	OUTPUT piggle_ipo_error LANGUAGES C CXX)
if(NOT piggle_ipo_supported)
	message(STATUS "Release IPO unavailable: ${piggle_ipo_error}")
endif()

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
	set(piggle_top_level ON)
else()
	set(piggle_top_level OFF)
endif()
option(BUILD_SHARED_LIBS "Build Piggle as a shared library" OFF)
option(PIGGLE_BUILD_CLI "Build the piggle archive command"
	${piggle_top_level})
option(BUILD_TESTING "Build Piggle tests" ${piggle_top_level})
include(CTest)
option(BUILD_UNIT_TESTS "Build GoogleTest unit tests" ON)
option(BUILD_ACCEPTANCE_TESTS "Build Cucumber acceptance tests" OFF)

if(BUILD_TESTING AND BUILD_ACCEPTANCE_TESTS)
	include(cmake/AcceptancePrerequisites.cmake)
endif()
add_subdirectory(cmake/dependencies)
include(cmake/runtime-foundation.cmake)
add_subdirectory(src)
if(PIGGLE_BUILD_CLI)
	add_subdirectory(cli)
endif()
if(BUILD_TESTING AND (BUILD_UNIT_TESTS OR BUILD_ACCEPTANCE_TESTS))
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
