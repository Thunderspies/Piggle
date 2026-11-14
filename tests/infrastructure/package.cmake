cmake_minimum_required(VERSION 3.24)

function(run)
	execute_process(COMMAND ${ARGV} RESULT_VARIABLE result
		OUTPUT_VARIABLE output ERROR_VARIABLE error)
	if(NOT result EQUAL 0)
		message(FATAL_ERROR "${ARGV}\n${output}\n${error}")
	endif()
endfunction()

set(work "${BUILD_DIR}/package-regressions")
file(REMOVE_RECURSE "${work}")
set(prefix "${work}/piggle")
run("${CMAKE_COMMAND}" --install "${BUILD_DIR}"
	--config "${CONFIG}" --prefix "${prefix}")
file(GLOB_RECURSE installed LIST_DIRECTORIES FALSE "${prefix}/*")
if(NOT INSTALL_DEPENDENCIES)
	foreach(path IN LISTS installed)
		file(RELATIVE_PATH relative "${prefix}" "${path}")
		if(relative MATCHES "(^|/)(zlib|zconf|libz-ng)")
			message(FATAL_ERROR "Unexpected dependency installation: ${path}")
		endif()
	endforeach()
elseif(NOT ZLIB_IMPORTED)
	list(FILTER installed INCLUDE REGEX "/zlib-ng-config\\.cmake$")
	if(NOT installed)
		message(FATAL_ERROR "Bundled install must include zlib-ng's package")
	endif()
endif()

find_program(cli NAMES piggle PATHS "${prefix}/bin" NO_DEFAULT_PATH)
if(cli)
	run("${cli}" --version)
	run("${cli}" --help)
endif()

set(consumer "${CMAKE_CURRENT_LIST_DIR}/fixtures/package")
set(configure "${CMAKE_COMMAND}" -S "${consumer}"
	-G "${GENERATOR}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
	"-DCMAKE_C_COMPILER=${C_COMPILER}"
	"-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
execute_process(COMMAND ${configure} -B "${work}/without-zlib"
	"-DCMAKE_PREFIX_PATH=${prefix}"
	-DCMAKE_DISABLE_FIND_PACKAGE_zlib-ng=TRUE
	RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(SHARED)
	if(NOT result EQUAL 0)
		message(FATAL_ERROR "Shared package requires zlib-ng: ${output}${error}")
	endif()
else()
	if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "zlib-ng")
		message(FATAL_ERROR "Static package must require zlib-ng")
	endif()
endif()

set(dependency "${work}/dependency")
if(NOT SHARED AND NOT INSTALL_DEPENDENCIES AND NOT ZLIB_IMPORTED)
	run("${CMAKE_COMMAND}" --install "${ZLIB_BINARY_DIR}"
		--config "${CONFIG}" --prefix "${dependency}")
endif()
set(prefixes "${prefix};${dependency};${PREFIX_PATH}")
execute_process(COMMAND ${configure} -B "${work}/consumer"
	"-DCMAKE_PREFIX_PATH=${prefixes}"
	RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
	message(FATAL_ERROR "Consumer configure failed: ${output}${error}")
endif()
run("${CMAKE_COMMAND}" --build "${work}/consumer" --config "${CONFIG}")
run("${CMAKE_CTEST_COMMAND}" --test-dir "${work}/consumer"
	-C "${CONFIG}" --output-on-failure)

file(REMOVE_RECURSE "${work}")
