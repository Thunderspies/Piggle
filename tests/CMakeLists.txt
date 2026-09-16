add_executable(piggle_api_unit api_unit.c)
target_link_libraries(piggle_api_unit PRIVATE piggle::piggle
	zlib-ng::zlib)
add_executable(piggle_api_invalid api_invalid.c)
target_link_libraries(piggle_api_invalid PRIVATE piggle::piggle)
add_executable(piggle_api_arguments api_arguments.c)
target_link_libraries(piggle_api_arguments PRIVATE piggle::piggle)
foreach(case IN ITEMS context source tree file reader writer descriptor
		named transfer pack builder unpack foreign)
	set(case_dir "${CMAKE_CURRENT_BINARY_DIR}/arguments_${case}")
	file(MAKE_DIRECTORY "${case_dir}")
	add_test(NAME api_arguments_${case}
		COMMAND piggle_api_arguments ${case})
	set_tests_properties(api_arguments_${case} PROPERTIES
		WORKING_DIRECTORY "${case_dir}" LABELS contract)
endforeach()
add_executable(piggle_api_contract api_contract.c)
target_link_libraries(piggle_api_contract PRIVATE piggle::piggle)
foreach(case IN ITEMS options names closes reads readers metadata tree)
	set(case_dir "${CMAKE_CURRENT_BINARY_DIR}/contract_${case}")
	file(MAKE_DIRECTORY "${case_dir}")
	add_test(NAME api_contract_${case} COMMAND piggle_api_contract ${case})
	set_tests_properties(api_contract_${case} PROPERTIES
		WORKING_DIRECTORY "${case_dir}" LABELS contract)
endforeach()
add_executable(piggle_api_state api_state.c)
target_link_libraries(piggle_api_state PRIVATE piggle::piggle)
foreach(case IN ITEMS native source file pigg hogg metadata_pigg metadata_hogg
		stale_pigg stale_hogg read_only delete unpack)
	set(case_dir "${CMAKE_CURRENT_BINARY_DIR}/state_${case}")
	file(MAKE_DIRECTORY "${case_dir}")
	add_test(NAME api_state_${case} COMMAND piggle_api_state ${case})
	set_tests_properties(api_state_${case} PROPERTIES
		WORKING_DIRECTORY "${case_dir}" LABELS contract)
endforeach()
add_executable(piggle_api_watch api_watch.c)
target_link_libraries(piggle_api_watch PRIVATE piggle::piggle)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/watch_contract")
add_test(NAME api_watch_contract COMMAND piggle_api_watch)
set_tests_properties(api_watch_contract PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/watch_contract"
	LABELS contract)
add_executable(piggle_api_roundtrip api_roundtrip.c)
target_link_libraries(piggle_api_roundtrip PRIVATE piggle::piggle)
foreach(case IN ITEMS source_pigg source_hogg tree_pigg tree_hogg
		empty_pigg empty_hogg)
	set(case_dir "${CMAKE_CURRENT_BINARY_DIR}/roundtrip_${case}")
	file(MAKE_DIRECTORY "${case_dir}")
	add_test(NAME api_roundtrip_${case}
		COMMAND piggle_api_roundtrip ${case})
	set_tests_properties(api_roundtrip_${case} PROPERTIES
		WORKING_DIRECTORY "${case_dir}" LABELS contract)
endforeach()
add_executable(piggle_api_flows api_flows.c)
target_link_libraries(piggle_api_flows PRIVATE piggle::piggle)
add_executable(piggle_api_stream api_stream.c)
target_link_libraries(piggle_api_stream PRIVATE piggle::piggle)
add_executable(piggle_api_discovery api_discovery.c)
target_link_libraries(piggle_api_discovery PRIVATE piggle::piggle)
add_executable(piggle_api_regressions api_regressions.c)
target_link_libraries(piggle_api_regressions PRIVATE piggle::piggle
	zlib-ng::zlib)
set(journal_cases)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	add_library(piggle_journal_fault SHARED journal_fault.c)
	target_link_libraries(piggle_api_regressions PRIVATE
		piggle_journal_fault)
	list(APPEND journal_cases journal_1 journal_2 journal_3 journal_4)
endif()
set(windows_path_cases)
if(WIN32)
	list(APPEND windows_path_cases windows_absolute windows_unicode
		windows_native)
	if(TARGET piggle_cli)
		target_compile_definitions(piggle_api_regressions PRIVATE
			PIGGLE_TEST_CLI_PATH="$<TARGET_FILE:piggle_cli>")
		add_dependencies(piggle_api_regressions piggle_cli)
		list(APPEND windows_path_cases windows_cli)
	endif()
endif()
set(loose_collision_cases)
if(UNIX)
	list(APPEND loose_collision_cases loose_collisions posix_basename)
endif()
foreach(case IN ITEMS retained_unpack pigg_controls hogg_controls
		loose_writer_ancestor ${windows_path_cases} scan_baseline
		native_baseline scan_rollback native_rollback watch_repair
		watch_worker short_datalist compressed_datalist ${journal_cases}
		normalized ancestor ${loose_collision_cases} empty_cursor
		exhausted_cursor loose_lease loose_alias worker_reader
		worker_cursor pigg_writer pigg_selected hogg_writer
		hogg_selected
		source_hierarchy source_prefix tree_hierarchy tree_prefix
		pigg_hierarchy hogg_hierarchy export_digest)
	set(case_dir "${CMAKE_CURRENT_BINARY_DIR}/regression_${case}")
	file(MAKE_DIRECTORY "${case_dir}")
	add_test(NAME regression_${case} COMMAND piggle_api_regressions ${case})
	set_tests_properties(regression_${case} PROPERTIES
		WORKING_DIRECTORY "${case_dir}" LABELS contract)
endforeach()
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_CROSSCOMPILING AND
	CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
	target_compile_definitions(piggle_api_discovery PRIVATE
		PIGGLE_TEST_WINE_CASES)
endif()

foreach(case IN ITEMS options buffer context names native_reader archive_read
		native_writer source tree builder pigg_edit hogg_delete
		hogg_update hogg_flush hogg_resize archive_copy archive_streaming
		native_encoded_streaming)
	set(case_dir "${CMAKE_CURRENT_BINARY_DIR}/${case}")
	file(MAKE_DIRECTORY "${case_dir}")
	add_test(NAME api_${case} COMMAND piggle_api_unit ${case})
	set_tests_properties(api_${case} PROPERTIES
		WORKING_DIRECTORY "${case_dir}"
		LABELS contract)
endforeach()

foreach(group IN ITEMS context io file source tree change archive_builder)
	add_test(NAME api_invalid_${group} COMMAND piggle_api_invalid ${group})
	set_tests_properties(api_invalid_${group} PROPERTIES LABELS contract)
endforeach()

set(extra_watch_cases)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	list(APPEND extra_watch_cases watch_retry)
endif()

foreach(case IN ITEMS readers_files writers_deletes writer_excess
		builder_workflows source_workflows source_delete
		source_delete_blocking tree_workflows watch_loss
		watch_reader watch_queue ${extra_watch_cases})
	set(case_dir "${CMAKE_CURRENT_BINARY_DIR}/${case}")
	file(MAKE_DIRECTORY "${case_dir}/root" "${case_dir}/out")
	add_test(NAME api_${case} COMMAND piggle_api_flows ${case})
	set_tests_properties(api_${case} PROPERTIES
		WORKING_DIRECTORY "${case_dir}"
		LABELS contract)
endforeach()

foreach(case IN ITEMS coroutines guards)
	set(case_dir "${CMAKE_CURRENT_BINARY_DIR}/stream_${case}")
	file(MAKE_DIRECTORY "${case_dir}")
	add_test(NAME api_stream_${case} COMMAND piggle_api_stream ${case})
	set_tests_properties(api_stream_${case} PROPERTIES
		WORKING_DIRECTORY "${case_dir}" LABELS contract)
endforeach()

set(discovery_cases source tree bulk source_scope)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	list(APPEND discovery_cases windows_scan)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	list(APPEND discovery_cases writer_guard native_query)
endif()
foreach(case IN LISTS discovery_cases)
	set(case_dir "${CMAKE_CURRENT_BINARY_DIR}/discovery_${case}")
	file(MAKE_DIRECTORY "${case_dir}")
	if(case STREQUAL "windows_scan" AND CMAKE_CROSSCOMPILING AND
		CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
		set(wine_case_dir "${case_dir}/root/window")
		file(MAKE_DIRECTORY "${wine_case_dir}")
		file(WRITE "${wine_case_dir}/Dup" "")
		file(WRITE "${wine_case_dir}/dup" "")
	endif()
	add_test(NAME api_discovery_${case}
		COMMAND piggle_api_discovery ${case})
	set_tests_properties(api_discovery_${case} PROPERTIES
		WORKING_DIRECTORY "${case_dir}" LABELS contract)
endforeach()

add_executable(piggle_api_cpp api_cpp.cpp)
target_link_libraries(piggle_api_cpp PRIVATE piggle::piggle)
add_test(NAME api_cpp_linkage COMMAND piggle_api_cpp)
set_tests_properties(api_cpp_linkage PROPERTIES LABELS infrastructure)

if(UNIX)
	add_executable(piggle_api_memory api_memory.c)
	target_link_libraries(piggle_api_memory PRIVATE piggle::piggle)
	foreach(format IN ITEMS pigg hogg)
		set(case_dir "${CMAKE_CURRENT_BINARY_DIR}/memory_${format}")
		file(MAKE_DIRECTORY "${case_dir}")
		add_test(NAME api_memory_${format}
			COMMAND piggle_api_memory ${format})
		set_tests_properties(api_memory_${format} PROPERTIES
			WORKING_DIRECTORY "${case_dir}" LABELS contract)
	endforeach()
endif()
