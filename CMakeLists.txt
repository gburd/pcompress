cmake_minimum_required(VERSION 3.14)
project(pcompress VERSION 4.0.0 LANGUAGES C CXX ASM)

#
# CMake build for Pcompress - alternative to config/Makefile workflow.
#
# Usage:
#   mkdir build && cd build
#   cmake ..
#   make
#
# Options:
#   -DENABLE_DEBUG=ON          Enable debug build
#   -DDISABLE_ALLOCATOR=ON     Disable internal memory allocator
#   -DENABLE_DEBUG_STATS=ON    Enable verbose debug stats
#   -DDISABLE_WAVPACK=ON       Disable WavPack filter
#   -DDISABLE_ZSTD=ON          Disable Zstandard compression
#   -DWAVPACK_DIR=<path>       WavPack source tree path
#   -DWITH_OPENSSL=<path>      Custom OpenSSL prefix
#   -DNO_SSE_DETECT=ON         Skip SSE detection
#   -DNO_AVX_DETECT=ON         Skip AVX detection
#   -DCMAKE_INSTALL_PREFIX=<p> Installation prefix (default: /usr)
#

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- Options ----
option(ENABLE_DEBUG "Enable debug mode" OFF)
option(DISABLE_ALLOCATOR "Disable internal allocator" OFF)
option(ENABLE_DEBUG_STATS "Enable debug statistics" OFF)
option(DISABLE_WAVPACK "Disable WavPack filter" OFF)
option(DISABLE_ZSTD "Disable Zstandard compression" OFF)
option(NO_SSE_DETECT "Skip SSE detection" OFF)
option(NO_AVX_DETECT "Skip AVX detection" OFF)
set(WAVPACK_DIR "" CACHE PATH "Path to WavPack source tree")
set(WITH_OPENSSL "" CACHE PATH "Path to custom OpenSSL installation")

# ---- 64-bit check ----
include(CheckTypeSize)
check_type_size("void*" SIZEOF_VOIDP)
if(NOT SIZEOF_VOIDP EQUAL 8)
    message(FATAL_ERROR "Only 64-bit platforms are supported.")
endif()

# ---- Find required packages ----
if(WITH_OPENSSL)
    set(OPENSSL_ROOT_DIR "${WITH_OPENSSL}")
endif()
find_package(OpenSSL 1.1.1 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(BZip2 REQUIRED)
find_package(Threads REQUIRED)

# ---- Platform detection ----
set(IS_X86 OFF)
set(IS_ARM OFF)
set(IS_RISCV OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    set(IS_X86 ON)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(IS_ARM ON)
    set(NO_SSE_DETECT ON)
    set(NO_AVX_DETECT ON)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "riscv64")
    set(IS_RISCV ON)
    set(NO_SSE_DETECT ON)
    set(NO_AVX_DETECT ON)
endif()

# ---- Main library sources ----
set(MAIN_SRCS
    utils/utils.c allocator.c lzma_compress.c ppmd_compress.c
    adaptive_compress.c lzfx_compress.c lz4_compress.c none_compress.c
    utils/xxhash_base.c utils/heap.c utils/cpuid.c
    utils/cpu_features.c utils/cpu_features_x86_64.c
    utils/cpu_features_arm64.c utils/cpu_features_riscv.c
    filters/analyzer/analyzer.c
    meta_stream.c pcompress.c
)

set(CRYPTO_SRCS
    crypto/aes/crypto_aes.c crypto/scrypt/crypto_scrypt-nosse.c
    crypto/scrypt/sha256.c crypto/scrypt/crypto_aesctr.c
    crypto/crypto_utils.c crypto/sha2_utils.c crypto/sha3_utils.c
    crypto/xsalsa20/xsalsa20_xor.c crypto/xsalsa20/hsalsa_core.c
)

set(LZMA_SRCS
    lzma/LzmaEnc.c lzma/LzFind.c lzma/LzmaDec.c
    lzma/Threads.c lzma/LzFindMt.c
)

set(PPMD_SRCS lzma/Ppmd8.c lzma/Ppmd8Enc.c lzma/Ppmd8Dec.c)
set(CRC_SRCS lzma/crc64_fast.c lzma/crc64_table.c lzma/crc32_fast.c lzma/crc32_table.c)
set(LZFX_SRCS lzfx/lzfx.c)
set(LZ4_SRCS lz4/lib/lz4.c lz4/lib/lz4hc.c)

# ---- Zstandard (optional) ----
if(NOT DISABLE_ZSTD)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/zstd/lib/zstd.h)
        set(ZSTD_SRCS
            zstd/lib/common/debug.c zstd/lib/common/entropy_common.c
            zstd/lib/common/error_private.c zstd/lib/common/fse_decompress.c
            zstd/lib/common/pool.c zstd/lib/common/threading.c
            zstd/lib/common/xxhash.c zstd/lib/common/zstd_common.c
            zstd/lib/compress/fse_compress.c zstd/lib/compress/hist.c
            zstd/lib/compress/huf_compress.c zstd/lib/compress/zstd_compress.c
            zstd/lib/compress/zstd_compress_literals.c
            zstd/lib/compress/zstd_compress_sequences.c
            zstd/lib/compress/zstd_compress_superblock.c
            zstd/lib/compress/zstd_double_fast.c zstd/lib/compress/zstd_fast.c
            zstd/lib/compress/zstd_lazy.c zstd/lib/compress/zstd_ldm.c
            zstd/lib/compress/zstdmt_compress.c zstd/lib/compress/zstd_opt.c
            zstd/lib/compress/zstd_preSplit.c
            zstd/lib/decompress/huf_decompress.c
            zstd/lib/decompress/zstd_ddict.c
            zstd/lib/decompress/zstd_decompress_block.c
            zstd/lib/decompress/zstd_decompress.c
            zstd_compress.c
        )
        set(ZSTD_INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}/zstd/lib
            ${CMAKE_CURRENT_SOURCE_DIR}/zstd/lib/common
        )
        set(ZSTD_DEFS ENABLE_PC_ZSTD XXH_NAMESPACE=ZSTD_ ZSTD_MULTITHREAD)
        message(STATUS "Zstandard: enabled")
    else()
        message(WARNING "Zstandard source not found (zstd/lib/zstd.h missing). Disabling.")
        set(ZSTD_SRCS "")
        set(ZSTD_INCLUDES "")
        set(ZSTD_DEFS "")
    endif()
else()
    message(STATUS "Zstandard: disabled")
    set(ZSTD_SRCS "")
    set(ZSTD_INCLUDES "")
    set(ZSTD_DEFS "")
endif()

set(RABIN_SRCS rabin/rabin_dedup.c rabin/global/index.c rabin/global/dedupe_config.c)
set(BSDIFF_SRCS bsdiff/bsdiff.c bsdiff/bspatch.c bsdiff/rle_encoder.c)
set(LZP_SRCS filters/lzp/lzp.c)
set(DELTA2_SRCS filters/delta2/delta2.c)
set(TRANSP_SRCS filters/transpose/transpose.c)

set(ARCHIVE_SRCS
    archive/pc_archive.c archive/pc_arc_filter.c
    utils/phash/phash.c utils/phash/lookupa.c utils/phash/recycle.c
)

set(PJPG_SRCS
    filters/packjpg/aricoder.cpp filters/packjpg/bitops.cpp
    filters/packjpg/packjpg.cpp archive/pjpg_helper.cpp
)

set(PPNM_SRCS filters/packpnm/packpnm.cpp archive/ppnm_helper.cpp)
set(DISPACK_SRCS filters/dispack/dis.cpp archive/dispack_helper.cpp)
set(DICT_SRCS filters/dict/DictFilter.cpp)
set(ZLIB_COMPRESS_SRCS zlib_compress.c)
set(BZLIB_COMPRESS_SRCS bzip2_compress.c)

set(SKEIN_SRCS
    crypto/skein/SHA3api_ref.c crypto/skein/skein.c
    crypto/skein/skein_debug.c crypto/skein/skein_block.c
)

set(SHA2_SRCS crypto/sha2/sha512.c)

set(KECCAK_COMMON_SRCS
    crypto/keccak/genKAT.c crypto/keccak/KeccakDuplex.c
    crypto/keccak/KeccakNISTInterface.c crypto/keccak/KeccakSponge.c
)
set(KECCAK_SRCS ${KECCAK_COMMON_SRCS} crypto/keccak/KeccakF-1600-opt64.c)

set(XXHASH_SRCS utils/xxhash_sse4.c utils/xxhash_sse2.c)

set(BLAKE2_SRCS
    crypto/blake2/blake2b_sse2.c crypto/blake2/blake2b_ssse3.c
    crypto/blake2/blake2b_sse41.c crypto/blake2/blake2b_avx.c
    crypto/blake2/blake2bp_sse2.c crypto/blake2/blake2bp_ssse3.c
    crypto/blake2/blake2bp_sse41.c crypto/blake2/blake2bp_avx.c
)

set(CRYPTO_COMPAT_SRCS
    crypto/old/sha2_utils_old.c crypto/old/sha3_utils_old.c
)

set(LIBBSC_WRAP_SRC libbsc_compress.c)

# Collect all library sources
set(LIB_SRCS
    ${MAIN_SRCS} ${CRYPTO_SRCS} ${LZMA_SRCS} ${PPMD_SRCS} ${CRC_SRCS}
    ${LZFX_SRCS} ${LZ4_SRCS} ${RABIN_SRCS} ${BSDIFF_SRCS} ${LZP_SRCS}
    ${DELTA2_SRCS} ${TRANSP_SRCS} ${ARCHIVE_SRCS} ${PJPG_SRCS} ${PPNM_SRCS}
    ${DISPACK_SRCS} ${DICT_SRCS} ${ZLIB_COMPRESS_SRCS} ${BZLIB_COMPRESS_SRCS}
    ${SKEIN_SRCS} ${SHA2_SRCS} ${KECCAK_SRCS} ${XXHASH_SRCS} ${BLAKE2_SRCS}
    ${CRYPTO_COMPAT_SRCS} ${LIBBSC_WRAP_SRC} ${ZSTD_SRCS}
    crypto/xsalsa20/stream.c
)

# ---- Include directories ----
set(COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/lzma
    ${CMAKE_CURRENT_SOURCE_DIR}/lzfx
    ${CMAKE_CURRENT_SOURCE_DIR}/lz4/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/rabin
    ${CMAKE_CURRENT_SOURCE_DIR}/bsdiff
    ${CMAKE_CURRENT_SOURCE_DIR}/filters/lzp
    ${CMAKE_CURRENT_SOURCE_DIR}/crypto/skein
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    ${CMAKE_CURRENT_SOURCE_DIR}/crypto/sha2
    ${CMAKE_CURRENT_SOURCE_DIR}/crypto/scrypt
    ${CMAKE_CURRENT_SOURCE_DIR}/crypto/aes
    ${CMAKE_CURRENT_SOURCE_DIR}/crypto
    ${CMAKE_CURRENT_SOURCE_DIR}/rabin/global
    ${CMAKE_CURRENT_SOURCE_DIR}/crypto/keccak
    ${CMAKE_CURRENT_SOURCE_DIR}/filters/transpose
    ${CMAKE_CURRENT_SOURCE_DIR}/crypto/blake2
    ${CMAKE_CURRENT_SOURCE_DIR}/crypto/xsalsa20
    ${CMAKE_CURRENT_SOURCE_DIR}/archive
    ${CMAKE_CURRENT_SOURCE_DIR}/filters
    ${CMAKE_CURRENT_SOURCE_DIR}/filters/analyzer
    ${CMAKE_CURRENT_SOURCE_DIR}/filters/dispack
    ${CMAKE_CURRENT_SOURCE_DIR}/filters/packjpg
    ${CMAKE_CURRENT_SOURCE_DIR}/filters/packpnm
)

# ---- Compile definitions ----
set(COMMON_DEFS
    NODEFAULT_PROPS
    FILE_OFFSET_BITS=64
    _REENTRANT
    __USE_SSE_INTRIN__
    _LZMA_PROB32
    __HASH_COMPATIBILITY_
)

if(DISABLE_ALLOCATOR)
    list(APPEND COMMON_DEFS DEBUG_NO_SLAB)
endif()

if(ENABLE_DEBUG_STATS)
    list(APPEND COMMON_DEFS DEBUG_STATS)
endif()

if(NOT ENABLE_DEBUG)
    list(APPEND COMMON_DEFS NDEBUG)
endif()

# ---- Build libarchive (static, internal) ----
set(LIBARCHIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/archive/libarchive)
if(EXISTS ${LIBARCHIVE_DIR}/CMakeLists.txt)
    # If libarchive has its own CMake build
    set(ENABLE_BSDTAR OFF CACHE BOOL "" FORCE)
    set(ENABLE_BSDCPIO OFF CACHE BOOL "" FORCE)
    set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(${LIBARCHIVE_DIR} libarchive_build EXCLUDE_FROM_ALL)
else()
    # Fall back to building via external configure
    message(STATUS "libarchive: using pre-configured build")
endif()

# ---- Build libbsc ----
set(LIBBSC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/bsc)

# ---- Shared library ----
add_library(libpcompress SHARED ${LIB_SRCS})
set_target_properties(libpcompress PROPERTIES
    OUTPUT_NAME pcompress
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

target_include_directories(libpcompress PRIVATE
    ${COMMON_INCLUDES}
    ${OPENSSL_INCLUDE_DIR}
    ${LIBBSC_DIR}/libbsc
    ${LIBARCHIVE_DIR}/libarchive
    ${ZSTD_INCLUDES}
)

target_compile_definitions(libpcompress PRIVATE ${COMMON_DEFS} ENABLE_PC_LIBBSC ${ZSTD_DEFS})

target_compile_options(libpcompress PRIVATE
    -fno-strict-aliasing
    -Wall
    -pedantic
    $<$<COMPILE_LANGUAGE:C>:-std=gnu99>
)

if(ENABLE_DEBUG)
    target_compile_options(libpcompress PRIVATE -g -O -fno-omit-frame-pointer -fno-inline)
else()
    target_compile_options(libpcompress PRIVATE -O3)
endif()

# ---- Link libraries ----
target_link_libraries(libpcompress PRIVATE
    OpenSSL::Crypto
    ZLIB::ZLIB
    BZip2::BZip2
    Threads::Threads
    ${CMAKE_DL_LIBS}
    m
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(libpcompress PRIVATE rt)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_libraries(libpcompress PRIVATE iconv)
endif()

# ---- Executable ----
add_executable(pcompress main.c)
target_include_directories(pcompress PRIVATE ${COMMON_INCLUDES})
target_compile_definitions(pcompress PRIVATE ${COMMON_DEFS})
target_compile_options(pcompress PRIVATE
    -fno-strict-aliasing
    -Wall
    -pedantic
    $<$<COMPILE_LANGUAGE:C>:-std=gnu99>
)
target_link_libraries(pcompress PRIVATE libpcompress)

# ---- Install ----
install(TARGETS pcompress RUNTIME DESTINATION bin)
install(TARGETS libpcompress
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(FILES README.md
    DESTINATION share/doc/pcompress
    RENAME README
)
