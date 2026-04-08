cmake_minimum_required(VERSION 3.16)

if(POLICY CMP0074)
  cmake_policy(SET CMP0074 NEW)
endif()
if(POLICY CMP0077)
  cmake_policy(SET CMP0077 NEW)
endif()

# Fix "unrecognized escape" issues when passing CMAKE_MODULE_PATH on Windows.
file(TO_CMAKE_PATH "${CMAKE_MODULE_PATH}" CMAKE_MODULE_PATH)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

project(xrpl)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    # GCC-specific fixes
    add_compile_options(-Wno-unknown-pragmas -Wno-subobject-linkage)
    # -Wno-subobject-linkage can be removed when we upgrade GCC version to at least 13.3
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Clang-specific fixes
    add_compile_options(-Wno-unknown-warning-option) # Ignore unknown warning options
elseif(MSVC)
    # MSVC-specific fixes
    add_compile_options(/wd4068) # Ignore unknown pragmas
endif()

# make GIT_COMMIT_HASH define available to all sources
find_package(Git)
if(Git_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} --git-dir=${CMAKE_CURRENT_SOURCE_DIR}/.git rev-parse HEAD
        OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE gch)
    if(gch)
        set(GIT_COMMIT_HASH "${gch}")
        message(STATUS gch: ${GIT_COMMIT_HASH})
        add_definitions(-DGIT_COMMIT_HASH="${GIT_COMMIT_HASH}")
    endif()

    execute_process(COMMAND ${GIT_EXECUTABLE} --git-dir=${CMAKE_CURRENT_SOURCE_DIR}/.git rev-parse --abbrev-ref HEAD
        OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE gb)
    if(gb)
        set(GIT_BRANCH "${gb}")
        message(STATUS gb: ${GIT_BRANCH})
        add_definitions(-DGIT_BRANCH="${GIT_BRANCH}")
    endif()
endif() #git

if(thread_safety_analysis)
  add_compile_options(-Wthread-safety -D_LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS -DXRPL_ENABLE_THREAD_SAFETY_ANNOTATIONS)
  add_compile_options("-stdlib=libc++")
  add_link_options("-stdlib=libc++")
endif()

include (CheckCXXCompilerFlag)
include (FetchContent)
include (ExternalProject)
include (CMakeFuncs) # must come *after* ExternalProject b/c it overrides one function in EP
if (target)
  message (FATAL_ERROR "The target option has been removed - use native cmake options to control build")
endif ()

include(XrplSanity)
include(XrplVersion)
include(XrplSettings)
# this check has to remain in the top-level cmake
# because of the early return statement
if (packages_only)
  if (NOT TARGET rpm)
    message (FATAL_ERROR "packages_only requested, but targets were not created - is docker installed?")
  endif()
  return ()
endif ()
include(XrplCompiler)
include(XrplInterface)

option(only_docs "Include only the docs target?" FALSE)
include(XrplDocs)
if(only_docs)
  return()
endif()

###

include(deps/dilithium)
include(deps/Boost)
find_package(OpenSSL 1.1.1 REQUIRED)
set_target_properties(OpenSSL::SSL PROPERTIES
  INTERFACE_COMPILE_DEFINITIONS OPENSSL_NO_SSL2
)

add_subdirectory(external/antithesis-sdk)
add_subdirectory(external/libff)
add_library(libff::ff ALIAS ff)

# Silence libff compiler warnings
target_include_directories(ff SYSTEM PRIVATE
  ${CMAKE_SOURCE_DIR}/external/libff/src
  ${CMAKE_SOURCE_DIR}/external/libff/depends/xbyak/xbyak
  ${CMAKE_SOURCE_DIR}/external/libff/depends/ate-pairing/include
)
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  target_compile_options(ff PRIVATE -Wno-array-bounds -Wno-stringop-overflow -fno-strict-aliasing)
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(ff PRIVATE -Wno-array-bounds)
endif()

# GMP/GMPXX required by libff for finite field arithmetic
find_package(GMP CONFIG QUIET)
if (NOT TARGET GMP::gmp)
  find_package(PkgConfig QUIET)
  if (PkgConfig_FOUND)
    pkg_check_modules(PC_GMP QUIET gmp)
    pkg_check_modules(PC_GMPXX QUIET gmpxx)
  endif()
  find_path(GMP_INCLUDE_DIR NAMES gmp.h HINTS ${PC_GMP_INCLUDE_DIRS} ${PC_GMPXX_INCLUDE_DIRS})
  find_library(GMP_LIBRARY NAMES gmp HINTS ${PC_GMP_LIBRARY_DIRS})
  find_library(GMPXX_LIBRARY NAMES gmpxx HINTS ${PC_GMPXX_LIBRARY_DIRS} ${PC_GMP_LIBRARY_DIRS})
  if (GMP_INCLUDE_DIR AND GMP_LIBRARY)
    add_library(GMP::gmp UNKNOWN IMPORTED)
    set_target_properties(GMP::gmp PROPERTIES
      IMPORTED_LOCATION "${GMP_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${GMP_INCLUDE_DIR}")
  endif()
  if (GMP_INCLUDE_DIR AND GMPXX_LIBRARY)
    add_library(GMP::gmpxx UNKNOWN IMPORTED)
    set_target_properties(GMP::gmpxx PROPERTIES
      IMPORTED_LOCATION "${GMPXX_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${GMP_INCLUDE_DIR}"
      INTERFACE_LINK_LIBRARIES GMP::gmp)
  endif()
endif()
if (NOT TARGET GMP::gmp OR NOT TARGET GMP::gmpxx)
  message(FATAL_ERROR "GMP/GMPXX not found. Install libgmp-dev or set CMAKE_PREFIX_PATH.")
endif()

find_package(gRPC REQUIRED)
find_package(lz4 REQUIRED)
# Target names with :: are not allowed in a generator expression.
# We need to pull the include directories and imported location properties
# from separate targets.
find_package(LibArchive REQUIRED)
find_package(SOCI REQUIRED)
find_package(SQLite3 REQUIRED)

option(rocksdb "Enable RocksDB" ON)
if(rocksdb)
  find_package(RocksDB REQUIRED)
  set_target_properties(RocksDB::rocksdb PROPERTIES
    INTERFACE_COMPILE_DEFINITIONS XRPL_ROCKSDB_AVAILABLE=1
  )
  target_link_libraries(xrpl_libs INTERFACE RocksDB::rocksdb)
endif()

find_package(date REQUIRED)
find_package(ed25519 REQUIRED)
find_package(nudb REQUIRED)
find_package(secp256k1 REQUIRED)
find_package(wasmi REQUIRED)
find_package(xxHash REQUIRED)

target_link_libraries(xrpl_libs INTERFACE
  ed25519::ed25519
  lz4::lz4
  OpenSSL::Crypto
  OpenSSL::SSL
  secp256k1::secp256k1
  soci::soci
  SQLite::SQLite3
)

# Work around changes to Conan recipe for now.
if(TARGET nudb::core)
  set(nudb nudb::core)
elseif(TARGET NuDB::nudb)
  set(nudb NuDB::nudb)
else()
  message(FATAL_ERROR "unknown nudb target")
endif()
target_link_libraries(xrpl_libs INTERFACE ${nudb})

if(coverage)
  include(XrplCov)
endif()

set(PROJECT_EXPORT_SET XrplExports)
include(XrplCore)
include(XrplInstall)
include(XrplValidatorKeys)

if(tests)
  include(CTest)
  add_subdirectory(src/tests/libxrpl)
endif()
