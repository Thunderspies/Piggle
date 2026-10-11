set(CMAKE_CXX_STANDARD 17)

add_library(test_support INTERFACE)
target_link_libraries(test_support INTERFACE piggle::piggle GTest::gtest)
target_compile_definitions(test_support INTERFACE
	PIGGLE_TEST_ROOT="${PROJECT_BINARY_DIR}/test-work")
add_subdirectory(helpers)

if(BUILD_UNIT_TESTS)
	add_subdirectory(unit)
endif()

if(BUILD_ACCEPTANCE_TESTS)
	add_subdirectory(acceptance)
	add_subdirectory(infrastructure)
endif()
