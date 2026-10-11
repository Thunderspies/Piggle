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
