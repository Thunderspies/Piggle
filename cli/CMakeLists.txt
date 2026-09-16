add_executable(piggle_cli main.cpp)
if(piggle_ipo_supported)
	set_property(TARGET piggle_cli PROPERTY
		INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()
set_target_properties(piggle_cli PROPERTIES OUTPUT_NAME piggle
	CXX_EXTENSIONS OFF CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
target_link_libraries(piggle_cli PRIVATE piggle::piggle)
target_compile_definitions(piggle_cli PRIVATE
	PIGGLE_VERSION="${PROJECT_VERSION}")
if(UNIX)
	target_compile_definitions(piggle_cli PRIVATE _FILE_OFFSET_BITS=64)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_compile_definitions(piggle_cli PRIVATE _TIME_BITS=64)
	file(RELATIVE_PATH piggle_cli_relative_lib
		"${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
	set_target_properties(piggle_cli PROPERTIES
		INSTALL_RPATH "$ORIGIN/${piggle_cli_relative_lib}")
endif()
if(MSVC)
	target_compile_options(piggle_cli PRIVATE /W4 /Gd)
	target_compile_definitions(piggle_cli PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
	target_compile_options(piggle_cli PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(WIN32 AND BUILD_SHARED_LIBS)
	add_custom_command(TARGET piggle_cli POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:piggle> $<TARGET_FILE_DIR:piggle_cli>
		VERBATIM)
endif()
install(TARGETS piggle_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(MINGW)
	target_link_options(piggle_cli PRIVATE -municode)
endif()
