cmake_minimum_required (VERSION 3.30)

include(FetchContent)

if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project(tavl LANGUAGES CXX)

option(TAVL_BUILD_TESTS "Build the tavl test suite (doctest)" ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

add_library(tavl_options INTERFACE)
target_compile_features(tavl_options INTERFACE cxx_std_20)
target_compile_options(tavl_options INTERFACE
  $<$<CONFIG:Debug>:$<$<CXX_COMPILER_ID:MSVC>:/arch:AVX /MDd /Zi /Wall /wd5045 /wd4820 /wd4514 /GR- /permissive- /Zc:preprocessor>>
  $<$<CONFIG:Release>:$<$<CXX_COMPILER_ID:MSVC>:/arch:AVX /MD /Wall /wd5045 /wd4820 /O2 /GR- /permissive- /Zc:preprocessor>>
  $<$<CONFIG:MinSizeRel>:$<$<CXX_COMPILER_ID:MSVC>:/arch:AVX /MD /Wall /wd5045 /wd4820 /O2 /GR- /permissive- /Zc:preprocessor>>
  $<$<CONFIG:RelWithDebInfo>:$<$<CXX_COMPILER_ID:MSVC>:/arch:AVX /MDd /ZI /Wall /wd5045 /wd4820 /O2 /GR- /permissive- /Zc:preprocessor>>

  $<$<CONFIG:Debug>:$<$<CXX_COMPILER_ID:GNU,Clang>:-mavx -g -Wall -Wextra -fno-rtti -fno-inline>>
  $<$<CONFIG:Release>:$<$<CXX_COMPILER_ID:GNU,Clang>:-mavx -O3 -Wall -Wextra -fno-rtti -pipe -flto>>
  $<$<CONFIG:MinSizeRel>:$<$<CXX_COMPILER_ID:GNU,Clang>:-mavx -O3 -Wall -Wextra -fno-rtti -pipe -flto -s>>
  $<$<CONFIG:RelWithDebInfo>:$<$<CXX_COMPILER_ID:GNU,Clang>:-mavx -O3 -g -Wall -Wextra -fno-rtti -pipe -flto>>
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
target_link_libraries(tavl PUBLIC tavl_options PRIVATE tavl_no_exceptions)
target_include_directories(tavl PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${reflect_SOURCE_DIR}"
)

if (TAVL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
