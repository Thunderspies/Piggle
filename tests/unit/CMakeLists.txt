add_library(c11_consumer OBJECT ../consumers/c11.c)
target_link_libraries(c11_consumer PRIVATE piggle::piggle)
set_target_properties(c11_consumer PROPERTIES
	C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS OFF)
piggle_configure_target(c11_consumer)

add_executable(unit_tests main.cpp consumer.cpp transitions.cpp
	$<TARGET_OBJECTS:c11_consumer>
	api_builder.cpp api_storage.cpp api_arguments.cpp api_checksum.cpp api_contract.cpp api_cpp.cpp
	api_discovery.cpp api_entries.cpp api_flows.cpp api_invalid.cpp
	api_lease.cpp api_managed.cpp api_memory.cpp api_metadata.cpp
	api_regressions.cpp api_roundtrip.cpp api_seek.cpp api_state.cpp
	api_stream.cpp api_threads.cpp api_unit.cpp api_watch.cpp
)
target_link_libraries(unit_tests PRIVATE test_support zlib-ng::zlib
	Threads::Threads)
piggle_configure_target(unit_tests)
target_compile_definitions(unit_tests PRIVATE
	PIGGLE_TEST_LEASE_PATH="$<TARGET_FILE:lease_probe>")
add_dependencies(unit_tests lease_probe)
if(UNIX)
	target_compile_definitions(unit_tests PRIVATE
		PIGGLE_TEST_MEMORY_PATH="$<TARGET_FILE:memory_probe>")
	add_dependencies(unit_tests memory_probe)
endif()
if(TARGET journal_fault)
	target_link_libraries(unit_tests PRIVATE journal_fault)
endif()
if(WIN32 AND TARGET piggle_cli)
	target_compile_definitions(unit_tests PRIVATE
		PIGGLE_TEST_CLI_PATH="$<TARGET_FILE:piggle_cli>")
	add_dependencies(unit_tests piggle_cli)
endif()
if(WIN32 AND BUILD_SHARED_LIBS)
	add_custom_command(TARGET unit_tests POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:piggle> $<TARGET_FILE_DIR:unit_tests>)
endif()

include(register.cmake)

if(TARGET allocation_probe)
	target_compile_definitions(unit_tests PRIVATE
		PIGGLE_TEST_ALLOCATION_PATH="$<TARGET_FILE:allocation_probe>")
	add_dependencies(unit_tests allocation_probe)
endif()
