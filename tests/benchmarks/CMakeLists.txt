add_executable(archive_benchmark archive.cpp)
target_link_libraries(archive_benchmark PRIVATE piggle::piggle)
piggle_configure_target(archive_benchmark)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT BUILD_SHARED_LIBS)
	target_compile_definitions(archive_benchmark PRIVATE PIGGLE_MEASURE_ALLOCATIONS)
	target_link_options(archive_benchmark PRIVATE
		LINKER:--wrap=malloc LINKER:--wrap=calloc LINKER:--wrap=realloc
		LINKER:--wrap=pread64)
elseif(WIN32)
	target_link_libraries(archive_benchmark PRIVATE psapi)
endif()
if(BUILD_TESTING)
	foreach(count 10000 50000 100000)
		add_test(NAME benchmark_${count}
			COMMAND archive_benchmark ${count}
				"${CMAKE_CURRENT_BINARY_DIR}/benchmark-${count}.hogg")
		set_tests_properties(benchmark_${count} PROPERTIES LABELS benchmark)
	endforeach()
endif()
