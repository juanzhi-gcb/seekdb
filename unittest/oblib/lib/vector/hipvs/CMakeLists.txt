# Copyright (c) 2025 OceanBase.
# Licensed under the Apache License, Version 2.0.
cmake_minimum_required(VERSION 3.16)
project(seekdb_hipvs_contracts LANGUAGES CXX)
include(CTest)
# Fixture hnswlib symbols must not interpose VSAG's independently built fork.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN YES)
find_package(Threads REQUIRED)
set(VSAG_SDK "$ENV{VSAG_SDK}" CACHE PATH "VSAG SDK prefix containing include/vsag and lib/vsag_lib")
if(NOT VSAG_SDK)
  message(FATAL_ERROR "Set VSAG_SDK to the seekdb VSAG SDK prefix")
endif()
get_filename_component(SEEKDB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(HIPVS_SOURCES "${SEEKDB_ROOT}/src/oblib/lib/vector")
find_library(VSAG_LIBRARY NAMES vsag PATHS "${VSAG_SDK}/lib/vsag_lib" NO_DEFAULT_PATH REQUIRED)
add_library(hipvs_contract_core STATIC
  "${HIPVS_SOURCES}/hipvs_hnsw_import.cpp"
  "${HIPVS_SOURCES}/hipvs_batch.cpp")
target_compile_features(hipvs_contract_core PUBLIC cxx_std_17)
target_include_directories(hipvs_contract_core PUBLIC "${HIPVS_SOURCES}")
target_include_directories(hipvs_contract_core SYSTEM PUBLIC "${VSAG_SDK}/include")
target_link_libraries(hipvs_contract_core PUBLIC "${VSAG_LIBRARY}" Threads::Threads ${CMAKE_DL_LIBS})
# Keep Ubuntu t64 SONAME compatibility local to this test build.
if(EXISTS "/usr/lib/x86_64-linux-gnu/libaio.so.1t64" AND NOT EXISTS "/usr/lib/x86_64-linux-gnu/libaio.so.1")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime")
  file(CREATE_LINK "/usr/lib/x86_64-linux-gnu/libaio.so.1t64"
    "${CMAKE_CURRENT_BINARY_DIR}/runtime/libaio.so.1" SYMBOLIC)
  target_link_options(hipvs_contract_core INTERFACE
    "LINKER:-rpath,${CMAKE_CURRENT_BINARY_DIR}/runtime"
    "LINKER:-rpath-link,${CMAKE_CURRENT_BINARY_DIR}/runtime")
endif()
foreach(test hnsw_import_test batch_test build_options_test worker_protocol_test)
  add_executable(${test} "${test}.cpp")
  target_include_directories(${test} SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/hnswlib")
  target_link_libraries(${test} PRIVATE hipvs_contract_core)
  if(test STREQUAL "worker_protocol_test")
    add_test(NAME ${test} COMMAND ${test} $<TARGET_FILE:testonly_worker_bridge> $<TARGET_FILE:seekdb_hipvs_worker>)
  else()
    add_test(NAME ${test} COMMAND ${test})
  endif()
  set_tests_properties(${test} PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/runtime:$ENV{LD_LIBRARY_PATH}")
endforeach()

# CPU-only transport fixtures, explicitly excluded from GPU evidence.
add_library(testonly_worker_bridge SHARED testonly_worker_bridge.cpp)
target_include_directories(testonly_worker_bridge PRIVATE "${HIPVS_SOURCES}")
add_executable(seekdb_hipvs_worker "${SEEKDB_ROOT}/deps/hipvs/hipvs_worker.cpp")
target_compile_features(seekdb_hipvs_worker PRIVATE cxx_std_17)
target_include_directories(seekdb_hipvs_worker PRIVATE "${HIPVS_SOURCES}")
target_link_libraries(seekdb_hipvs_worker PRIVATE ${CMAKE_DL_LIBS})

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME worker_request_test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/worker_request_test.py"
  $<TARGET_FILE:seekdb_hipvs_worker> $<TARGET_FILE:testonly_worker_bridge>)
