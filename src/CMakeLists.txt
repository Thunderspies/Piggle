add_library(piggle
	api.cpp
	archive_builder.cpp
	change.cpp
	entries.cpp
	file.cpp
	io.cpp
	lease.cpp
	managed.cpp
	native_path.cpp
	source.cpp
	tree.cpp
	types.cpp
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
	$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
