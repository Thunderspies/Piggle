# The upstream cucumber-cpp library supplies the native wire-server main.
add_executable(acceptance_steps construction.cpp world.cpp names.cpp view.cpp entries.cpp
	discovery.cpp handles.cpp watch.cpp work.cpp edit.cpp io.cpp)
target_link_libraries(acceptance_steps PRIVATE cucumber-cpp test_support
	zlib-ng::zlib Threads::Threads)
piggle_configure_target(acceptance_steps)
if(TARGET journal_fault)
	target_link_libraries(acceptance_steps PRIVATE journal_fault)
endif()
if(WIN32 AND BUILD_SHARED_LIBS)
	add_custom_command(TARGET acceptance_steps POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:piggle> $<TARGET_FILE_DIR:acceptance_steps>)
endif()

add_test(NAME acceptance
	COMMAND "${RUBY_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run.rb"
		$<TARGET_FILE:acceptance_steps>
)
set_tests_properties(acceptance PROPERTIES
	LABELS acceptance
	WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
