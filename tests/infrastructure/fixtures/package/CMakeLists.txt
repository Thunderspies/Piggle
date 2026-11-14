cmake_minimum_required(VERSION 3.24)
project(package_consumer LANGUAGES C CXX)
find_package(piggle CONFIG REQUIRED)
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE piggle::piggle)
get_target_property(piggle_type piggle::piggle TYPE)
if(WIN32 AND piggle_type STREQUAL "SHARED_LIBRARY")
	add_custom_command(TARGET consumer POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:piggle::piggle> $<TARGET_FILE_DIR:consumer>)
endif()
enable_testing()
add_test(NAME consumer COMMAND consumer)
