# Apply diagnostics and optimization only to maintained targets.
function(piggle_configure_target target)
	if(MSVC)
		target_compile_definitions(${target} PRIVATE
			_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
		target_compile_options(${target} PRIVATE /W4 /WX /Gd /utf-8)
		target_link_options(${target} PRIVATE /WX)
	else()
		target_compile_options(${target} PRIVATE
			-Wall -Wextra -Wpedantic -Werror)
		target_link_options(${target} PRIVATE -Werror)
		if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR MINGW)
			target_link_options(${target} PRIVATE
				LINKER:--fatal-warnings)
			target_compile_options(${target} PRIVATE
				$<$<CONFIG:Release>:-ffunction-sections>
				$<$<CONFIG:Release>:-fdata-sections>)
			target_link_options(${target} PRIVATE
				$<$<CONFIG:Release>:LINKER:--gc-sections>)
		elseif(APPLE)
			target_link_options(${target} PRIVATE
				LINKER:-fatal_warnings)
		endif()
	endif()
	if(piggle_ipo_supported)
		set_property(TARGET ${target} PROPERTY
			INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
	endif()
endfunction()

function(piggle_configure_runtime target)
	find_package(Threads REQUIRED)
	target_compile_features(${target} PUBLIC c_std_11 cxx_std_17)
	set_target_properties(${target} PROPERTIES
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES
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
	piggle_configure_target(${target})
endfunction()
