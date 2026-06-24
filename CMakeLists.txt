cmake_minimum_required (VERSION 3.30)

include(FetchContent)

project(tavl LANGUAGES CXX)

option(TAVL_BUILD_TESTS "Build the tavl test suite (doctest)" ON)
# ISA for auto-vectorizing tavl's own sources. Applied PRIVATE; not forced on
# consumers (they pick their own arch). No SIMD intrinsics in headers, so no ABI impact.
set(TAVL_ARCH "AVX" CACHE STRING "ISA baseline for tavl sources: OFF, AVX, AVX2, NATIVE")

if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

if (NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

add_library(tavl_options INTERFACE)

set(TAVL_LTO_OPTION "-flto")
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(TAVL_LTO_OPTION "-flto=auto")
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(TAVL_LTO_OPTION "-flto=thin")
endif()

set(TAVL_ARCH_GNU "")
set(TAVL_ARCH_MSVC "")
if (TAVL_ARCH STREQUAL "AVX")
  set(TAVL_ARCH_GNU "-mavx")
  set(TAVL_ARCH_MSVC "/arch:AVX")
elseif (TAVL_ARCH STREQUAL "AVX2")
  set(TAVL_ARCH_GNU "-mavx2")
  set(TAVL_ARCH_MSVC "/arch:AVX2")
elseif (TAVL_ARCH STREQUAL "NATIVE")
  set(TAVL_ARCH_GNU "-march=native")
endif()

target_compile_options(tavl_options INTERFACE
  $<$<CONFIG:Debug>:$<$<CXX_COMPILER_ID:MSVC>:${TAVL_ARCH_MSVC} /Zi /Wall /wd5045 /wd4820 /wd4514 /GR- /permissive- /Zc:preprocessor>>
  $<$<CONFIG:Release>:$<$<CXX_COMPILER_ID:MSVC>:${TAVL_ARCH_MSVC} /Wall /wd5045 /wd4820 /O2 /GL /GR- /permissive- /Zc:preprocessor>>
  $<$<CONFIG:MinSizeRel>:$<$<CXX_COMPILER_ID:MSVC>:${TAVL_ARCH_MSVC} /Wall /wd5045 /wd4820 /O2 /GL /GR- /permissive- /Zc:preprocessor>>
  $<$<CONFIG:RelWithDebInfo>:$<$<CXX_COMPILER_ID:MSVC>:${TAVL_ARCH_MSVC} /Zi /Wall /wd5045 /wd4820 /O2 /GL /GR- /permissive- /Zc:preprocessor>>

  $<$<CONFIG:Debug>:$<$<CXX_COMPILER_ID:GNU,Clang>:${TAVL_ARCH_GNU} -g -Wall -Wextra -fno-rtti -fno-inline>>
  $<$<CONFIG:Release>:$<$<CXX_COMPILER_ID:GNU,Clang>:${TAVL_ARCH_GNU} -O3 -Wall -Wextra -fno-rtti -pipe ${TAVL_LTO_OPTION}>>
  $<$<CONFIG:MinSizeRel>:$<$<CXX_COMPILER_ID:GNU,Clang>:${TAVL_ARCH_GNU} -O3 -Wall -Wextra -fno-rtti -pipe ${TAVL_LTO_OPTION} -s>>
  $<$<CONFIG:RelWithDebInfo>:$<$<CXX_COMPILER_ID:GNU,Clang>:${TAVL_ARCH_GNU} -O3 -g -Wall -Wextra -fno-rtti -pipe ${TAVL_LTO_OPTION}>>

  # unstable in clang?
  $<$<AND:$<CONFIG:Release,MinSizeRel,RelWithDebInfo>,$<CXX_COMPILER_ID:GNU>>:-ffat-lto-objects>
)

target_link_options(tavl_options INTERFACE
  $<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:$<$<CXX_COMPILER_ID:GNU,Clang>:${TAVL_LTO_OPTION}>>
  $<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:$<$<CXX_COMPILER_ID:MSVC>:/LTCG>>
)

target_compile_definitions(tavl_options INTERFACE
  $<$<CONFIG:Debug>:>
  $<$<CONFIG:Release>:NDEBUG _NDEBUG>
  $<$<CONFIG:MinSizeRel>:NDEBUG _NDEBUG>
  $<$<CONFIG:RelWithDebInfo>:>
)

FetchContent_Declare(
  reflect
  GIT_REPOSITORY https://github.com/qlibs/reflect.git
  GIT_TAG        v1.2.6
  GIT_PROGRESS   TRUE
  FIND_PACKAGE_ARGS
)
FetchContent_MakeAvailable(reflect)

set(TAVL_HDR
  include/tavl/common.h
  include/tavl/detail.h
  include/tavl/lexer.h
  include/tavl/parser.h
  include/tavl/type_traits.h
  include/tavl/ext.h
  include/tavl/deserialize.h
  include/tavl/serialize.h
  include/tavl/tavl.h
)

set(TAVL_SRC
  src/detail.cpp
  src/lexer.cpp
  src/parser.cpp
  src/ext.cpp
  src/deserialize.cpp
)

add_library(tavl_no_exceptions INTERFACE)
target_compile_options(tavl_no_exceptions INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-fno-exceptions>
  $<$<CXX_COMPILER_ID:MSVC>:/EHs-c->
)

add_library(tavl STATIC ${TAVL_SRC} ${TAVL_HDR})
target_link_libraries(tavl PRIVATE tavl_options tavl_no_exceptions)
target_compile_features(tavl PUBLIC cxx_std_20)
set_target_properties(tavl PROPERTIES
  STATIC_LIBRARY_OPTIONS "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release,MinSizeRel,RelWithDebInfo>>:/LTCG>"
)
target_include_directories(tavl PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${reflect_SOURCE_DIR}"
)

if (TAVL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
