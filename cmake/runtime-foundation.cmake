# Shared build foundation for skeletons and later implementations.
if(NOT COMMAND CPMAddPackage)
	include("${CMAKE_CURRENT_LIST_DIR}/CPM.cmake")
endif()

# Keep zlib-ng static, including when Piggle itself is a shared library.
set(piggle_zlib_ng_hash
	f9c65aa9c852eb8255b636fd9f07ce1c406f061ec19a2e7d508b318ca0c907d1
)
set(piggle_zlib_ng_platform_options)
if(MINGW AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
	# GCC PR54412 can misalign wide-vector stack slots on Windows.
	# Disabling AVX2 also disables its dependent AVX512 variants.
	list(APPEND piggle_zlib_ng_platform_options "WITH_AVX2 OFF")
endif()
CPMAddPackage(
	NAME zlib-ng
	VERSION 2.3.3
	URL https://github.com/zlib-ng/zlib-ng/archive/refs/tags/2.3.3.tar.gz
	URL_HASH SHA256=${piggle_zlib_ng_hash}
	OPTIONS
		"BUILD_SHARED_LIBS OFF"
		"CMAKE_POSITION_INDEPENDENT_CODE ON"
		"ZLIB_COMPAT OFF"
		"ZLIB_ALIASES OFF"
		"BUILD_TESTING OFF"
		${piggle_zlib_ng_platform_options}
)

function(piggle_configure_runtime target)
	find_package(Threads REQUIRED)
	target_compile_features(${target} PUBLIC c_std_11 cxx_std_11)
	set_target_properties(${target} PROPERTIES
		CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES
		CXX_EXTENSIONS OFF)
	if(NOT WIN32)
		set_target_properties(${target} PROPERTIES
			CXX_VISIBILITY_PRESET hidden
			VISIBILITY_INLINES_HIDDEN YES)
	endif()
	target_compile_definitions(${target} PRIVATE PIGGLE_BUILD)
	if(UNIX)
		target_compile_definitions(${target}
			PRIVATE _FILE_OFFSET_BITS=64)
	endif()
	target_link_libraries(${target} PRIVATE zlib-ng::zlib Threads::Threads)
	if(WIN32)
		target_link_libraries(${target} PRIVATE ntdll)
	endif()
	if(BUILD_SHARED_LIBS)
		target_compile_definitions(${target} PUBLIC PIGGLE_SHARED)
		if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
			target_link_options(${target} PRIVATE
				"LINKER:--exclude-libs,ALL")
		endif()
	endif()
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4 /Gd)
	else()
		target_compile_options(${target}
			PRIVATE -Wall -Wextra -Wpedantic)
	endif()
endfunction()
