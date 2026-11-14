add_executable(fixture_steps fixtures/steps.cpp)
target_link_libraries(fixture_steps PRIVATE cucumber-cpp GTest::gtest)

add_executable(fixture_server fixtures/server.cpp)
add_executable(fixture_unit_empty ../unit/main.cpp)
target_link_libraries(fixture_unit_empty PRIVATE GTest::gtest)
add_executable(fixture_unit_cases ../unit/main.cpp fixtures/unit.cpp)
target_link_libraries(fixture_unit_cases PRIVATE GTest::gtest)

add_test(NAME runner_regressions
	COMMAND "${RUBY_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run.rb"
		$<TARGET_FILE:fixture_steps> $<TARGET_FILE:fixture_server>
		$<TARGET_FILE:fixture_unit_empty> $<TARGET_FILE:fixture_unit_cases>
		"${CMAKE_COMMAND}" "${CMAKE_CTEST_COMMAND}" "${CMAKE_GENERATOR}"
)
set_tests_properties(runner_regressions PROPERTIES
	LABELS infrastructure
	TIMEOUT 300
	WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)

get_target_property(package_zlib_binary_dir zlib-ng::zlib BINARY_DIR)
get_target_property(package_zlib_imported zlib-ng::zlib IMPORTED)
add_test(NAME package_regressions
	COMMAND "${CMAKE_COMMAND}"
		"-DBUILD_DIR=${PROJECT_BINARY_DIR}"
		"-DCONFIG=$<CONFIG>"
		"-DSHARED=${BUILD_SHARED_LIBS}"
		"-DINSTALL_DEPENDENCIES=${PIGGLE_INSTALL_DEPENDENCIES}"
		"-DZLIB_BINARY_DIR=${package_zlib_binary_dir}"
		"-DZLIB_IMPORTED=${package_zlib_imported}"
		"-DPREFIX_PATH=${CMAKE_PREFIX_PATH}"
		"-DGENERATOR=${CMAKE_GENERATOR}"
		"-DC_COMPILER=${CMAKE_C_COMPILER}"
		"-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/package.cmake"
)
set_tests_properties(package_regressions PROPERTIES
	LABELS infrastructure TIMEOUT 120)
