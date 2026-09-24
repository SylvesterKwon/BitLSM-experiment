cmake_minimum_required(VERSION 3.14)

project(BitLSM-experiment VERSION 0.1 LANGUAGES CXX)

# 빌드 후 실행 파일이 생성될 위치를 'bin' 디렉터리로 지정
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# C++ Standard
set(CMAKE_CXX_STANDARD 20)

# Build Type & Compiler Flags Preferences
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O3 -march=native -mtune=native -g -fno-omit-frame-pointer -DNDEBUG")

string(TOUPPER "${CMAKE_BUILD_TYPE}" UPPER_BUILD_TYPE)
message(STATUS "=======================================================")
message(STATUS "Current Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Active CXX Flags : ${CMAKE_CXX_FLAGS_${UPPER_BUILD_TYPE}}")
message(STATUS "=======================================================")

# gflags (RocksDB 도구류에서 사용)
find_package(gflags REQUIRED)

# 코어 라이브러리 (rocksdb, roaring 포함; folly TDigest는 BitLSM 내부에 vendoring)
add_subdirectory(third_party/BitLSM)

# 실험 전용 의존성
add_compile_definitions(CXXOPTS_NO_RTTI)
include(FetchContent)

## CXXOPTS
FetchContent_Declare(
  cxxopts
  GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
  GIT_TAG        v3.3.1
)
FetchContent_MakeAvailable(cxxopts)

## nlohmann/json (header-only)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

## simdjson
FetchContent_Declare(
  simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson.git
  GIT_TAG        v3.12.2
)
FetchContent_MakeAvailable(simdjson)

# 시스템 의존성
find_library(JEMALLOC_LIB jemalloc REQUIRED)
find_package(Threads REQUIRED)

# 공통 링크 라이브러리
set(COMMON_LINK_LIBS
    bitlsm::bitlsm
    roaring
    rocksdb
    dl
    Threads::Threads
    ${JEMALLOC_LIB}
    m
    z
    bz2
    snappy
    lz4
    zstd
)

# EXPERIMENT BINDINGS (정적 라이브러리)
file(GLOB BINDING_SOURCES "src/bindings/*.cpp" "src/bindings/embedded/*.cpp" "src/bindings/sai/*.cpp" "src/bindings/global_bins/*.cpp" "src/bindings/lazy_bitmaps/*.cpp")
add_library(experiment_bindings STATIC ${BINDING_SOURCES})

# Path to the patched RocksDB vendored inside BitLSM. Referenced DIRECTLY so this
# baseline does not depend on bitlsm::bitlsm exporting its internal include dirs
# (those will be made private).
set(EMBEDDED_ROCKSDB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/BitLSM/third_party/rocksdb)

target_include_directories(experiment_bindings PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bindings
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bindings/embedded
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bindings/sai
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bindings/global_bins
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bindings/lazy_bitmaps
    ${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark
    ${EMBEDDED_ROCKSDB_DIR}
    ${EMBEDDED_ROCKSDB_DIR}/include
)
# RocksDB is built -fno-rtti; TUs including its internal headers must match.
target_compile_options(experiment_bindings PRIVATE -fno-rtti)
target_link_libraries(experiment_bindings PUBLIC
    ${COMMON_LINK_LIBS}
    simdjson
    cxxopts::cxxopts
    nlohmann_json::nlohmann_json
)

# EXPERIMENTS
file(GLOB_RECURSE EXPERIMENT_SOURCES "src/benchmark/*.cpp" "src/honk_player/*.cpp")
list(FILTER EXPERIMENT_SOURCES EXCLUDE REGEX ".*/bindings/.*")
foreach(EXP_SRC ${EXPERIMENT_SOURCES})
    get_filename_component(EXP_NAME ${EXP_SRC} NAME_WE)
    add_executable(${EXP_NAME} ${EXP_SRC})
    get_filename_component(EXP_DIR ${EXP_SRC} DIRECTORY)
    get_filename_component(EXP_PARENT_DIR ${EXP_DIR} DIRECTORY)

    target_include_directories(${EXP_NAME} PRIVATE
        ${EXP_DIR}
        ${EXP_PARENT_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bindings
    )
    target_link_directories(${EXP_NAME} PRIVATE /usr/local/lib)
    target_link_libraries(${EXP_NAME} PRIVATE ${COMMON_LINK_LIBS} experiment_bindings)

    message(STATUS "Experiment executable added: ${EXP_NAME}")
endforeach()

# TOOLS (offline inspectors; reach into RocksDB internals like the bindings do)
file(GLOB TOOL_SOURCES "src/tools/*.cpp")
foreach(TOOL_SRC ${TOOL_SOURCES})
    get_filename_component(TOOL_NAME ${TOOL_SRC} NAME_WE)
    add_executable(${TOOL_NAME} ${TOOL_SRC})
    target_include_directories(${TOOL_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bindings
        ${CMAKE_CURRENT_SOURCE_DIR}/src/honk_player
        ${EMBEDDED_ROCKSDB_DIR}
        ${EMBEDDED_ROCKSDB_DIR}/include
    )
    target_link_directories(${TOOL_NAME} PRIVATE /usr/local/lib)
    target_link_libraries(${TOOL_NAME} PRIVATE ${COMMON_LINK_LIBS} experiment_bindings)
    message(STATUS "Tool executable added: ${TOOL_NAME}")
endforeach()

# SAI unit-test executables (assert-based standalone mains)
file(GLOB SAI_TEST_SOURCES "src/bindings/sai/test/*.cpp")
foreach(TEST_SRC ${SAI_TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SRC})
    target_include_directories(${TEST_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bindings
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bindings/sai
        ${EMBEDDED_ROCKSDB_DIR}
        ${EMBEDDED_ROCKSDB_DIR}/include
    )
    target_compile_options(${TEST_NAME} PRIVATE -fno-rtti -UNDEBUG)
    target_link_libraries(${TEST_NAME} PRIVATE ${COMMON_LINK_LIBS} experiment_bindings)
    message(STATUS "SAI test executable added: ${TEST_NAME}")
endforeach()

# Lazy Bitmaps unit-test executables (assert-based standalone mains)
file(GLOB LAZY_BITMAPS_TEST_SOURCES "src/bindings/lazy_bitmaps/test/*.cpp")
foreach(TEST_SRC ${LAZY_BITMAPS_TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SRC})
    target_include_directories(${TEST_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bindings
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bindings/lazy_bitmaps
        ${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark
        ${EMBEDDED_ROCKSDB_DIR}
        ${EMBEDDED_ROCKSDB_DIR}/include
    )
    target_compile_options(${TEST_NAME} PRIVATE -fno-rtti -UNDEBUG)
    target_link_libraries(${TEST_NAME} PRIVATE ${COMMON_LINK_LIBS} experiment_bindings)
    message(STATUS "Lazy Bitmaps test executable added: ${TEST_NAME}")
endforeach()
