add_subdirectory(simsimd)
add_subdirectory(hdrhistogram_c)
add_subdirectory(hnswlib)
add_subdirectory(snowball)
add_subdirectory(icu)

# SVS C API, x86_64 Linux only; elsewhere vector_svs.cc compiles as a stub.
# Default: compile the public C API from the svs submodule (no LVQ/LeanVec).
# SVS_C_API_PREBUILT links the proprietary prebuilt archive instead. The C ABI
# is identical in both, so LVQ/LeanVec availability is probed at runtime.
option(SVS_C_API_PREBUILT
       "Link the prebuilt SVS C API (LVQ/LeanVec) instead of compiling third_party/svs" OFF)
# TODO: switch to a versioned release once one ships svs-c-api-no-omp.
set(SVS_C_API_URL
    "https://github.com/intel/ScalableVectorSearch/releases/download/nightly/svs-c-api-no-omp-2026-11-02-152.tar.gz"
    CACHE STRING "Prebuilt SVS C API tarball: a release URL or a local path")
set(SVS_C_API_URL_HASH
    "SHA256=c8df267369eb408b272f02c958e33fcc8d8a9b52553788b5a5667d90be118d61"
    CACHE STRING "Hash of SVS_C_API_URL; empty skips verification")
# Every SVS C API function the module calls; keep in sync with src/indexes.
set(SVS_C_API_REQUIRED_ENTRY_POINTS
    svs_algorithm_create_vamana svs_algorithm_free
    svs_algorithm_vamana_set_alpha svs_error_create svs_error_free
    svs_error_get_code svs_error_get_message svs_error_set svs_get_version
    svs_index_build_dynamic_ex svs_index_builder_create svs_index_builder_free
    svs_index_builder_set_allocator svs_index_builder_set_allocator_custom
    svs_index_builder_set_logger svs_index_builder_set_storage
    svs_index_builder_set_threadpool_custom svs_index_convert_dynamic_ex
    svs_index_dynamic_add_points svs_index_dynamic_compact
    svs_index_dynamic_consolidate svs_index_dynamic_delete_points
    svs_index_dynamic_get_sync_kind svs_index_free svs_index_get_memory_breakdown
    svs_index_get_memory_usage
    svs_index_get_size svs_index_load_dynamic_ex svs_index_load_stream_dynamic
    svs_index_save_stream
    svs_index_search_topk svs_logger_create_custom svs_logger_free
    svs_logger_set_level svs_search_params_create_vamana svs_search_params_free
    svs_search_results_free svs_set_default_logger svs_storage_create_leanvec
    svs_storage_create_lvq svs_storage_create_simple svs_storage_create_sq
    svs_storage_free)

if(NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux"
        AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"))
  if(SVS_C_API_PREBUILT)
    message(FATAL_ERROR "The prebuilt SVS C API requires x86_64 Linux")
  endif()
  message(STATUS "SVS C API: unavailable on this platform")
  return()
endif()

if(SVS_C_API_PREBUILT)
  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  set(_svs_fetch_args URL "${SVS_C_API_URL}")
  if(SVS_C_API_URL_HASH)
    list(APPEND _svs_fetch_args URL_HASH "${SVS_C_API_URL_HASH}")
  endif()
  FetchContent_Declare(svs_c_api ${_svs_fetch_args})
  FetchContent_MakeAvailable(svs_c_api)
  set(SVS_C_API_ROOT "${svs_c_api_SOURCE_DIR}")
  set(SVS_C_API_ARCHIVE "${SVS_C_API_ROOT}/lib/libsvs_c_api.a")
  if(NOT EXISTS "${SVS_C_API_ARCHIVE}")
    message(FATAL_ERROR "Prebuilt SVS C API: ${SVS_C_API_ARCHIVE} not found")
  endif()
  execute_process(
      COMMAND "${CMAKE_NM}" -g "${SVS_C_API_ARCHIVE}"
      COMMAND grep -E " (T svs_|U GOMP_)"
      OUTPUT_VARIABLE _svs_c_api_syms ERROR_QUIET)
  set(_svs_missing)
  foreach(_fn IN LISTS SVS_C_API_REQUIRED_ENTRY_POINTS)
    if(NOT _svs_c_api_syms MATCHES " T ${_fn}\n")
      list(APPEND _svs_missing ${_fn})
    endif()
  endforeach()
  if(_svs_missing)
    list(JOIN _svs_missing ", " _svs_missing)
    message(FATAL_ERROR
        "Prebuilt SVS C API ${SVS_C_API_ARCHIVE} lacks entry points the "
        "module calls: ${_svs_missing}. Use an archive built from SVS "
        "cde3771 or later.")
  endif()
  # libgomp must not enter DT_NEEDED (ci/check_svs_link.sh).
  if(_svs_c_api_syms MATCHES " U GOMP_")
    message(FATAL_ERROR
        "Prebuilt SVS C API ${SVS_C_API_ARCHIVE} was built with OpenMP. Use "
        "an archive built with SVS_ENABLE_OMP=OFF.")
  endif()
  # The archive bundles fmt/spdlog/MKL; --exclude-libs keeps those symbols out
  # of the module's dynamic table, independent of libsearch's own link flags.
  add_library(svs_c_api_static STATIC IMPORTED GLOBAL)
  set_target_properties(svs_c_api_static PROPERTIES
      IMPORTED_LOCATION "${SVS_C_API_ARCHIVE}"
      INTERFACE_INCLUDE_DIRECTORIES "${SVS_C_API_ROOT}/include"
      IMPORTED_LINK_INTERFACE_LANGUAGES CXX)
  # A link item, not INTERFACE_LINK_OPTIONS: those skip PRIVATE static deps
  # under CMP0099 OLD (cmake_minimum_required 3.16).
  target_link_libraries(svs_c_api_static INTERFACE ${CMAKE_DL_LIBS} m
      "-Wl,--exclude-libs,libsvs_c_api.a")
  target_compile_definitions(svs_c_api_static INTERFACE
      VALKEY_SEARCH_SVS_PREBUILT=1)
  message(STATUS "SVS C API: prebuilt (LVQ/LeanVec) from ${SVS_C_API_ROOT}")
else()
  # bindings/c is added EXCLUDE_FROM_ALL only so it generates svs_c_version.h;
  # its shared library stays out of the link set. It is still configured, so
  # OpenMP is switched off first or it would be required at configure time.
  set(SVS_ENABLE_OMP OFF CACHE BOOL "" FORCE)
  add_subdirectory(svs ${CMAKE_CURRENT_BINARY_DIR}/svs-build)
  add_subdirectory(svs/bindings/c ${CMAKE_CURRENT_BINARY_DIR}/svs-c-api-build
                   EXCLUDE_FROM_ALL)
  # The C API sources compile with SVS's strict warnings-as-errors flags
  # (svs_compile_options); a dedicated library keeps them off vector_svs.cc.
  set(SVS_C_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/svs/bindings/c")
  set(SVS_C_API_INCLUDE_DIR "${SVS_C_API_DIR}/include")
  set(SRCS_SVS_C_API
      ${SVS_C_API_DIR}/src/error.cpp
      ${SVS_C_API_DIR}/src/svs_c.cpp
      ${SVS_C_API_DIR}/src/dispatcher_vamana.cpp
      ${SVS_C_API_DIR}/src/dispatcher_dynamic_vamana.cpp
      ${SVS_C_API_DIR}/src/index_builder.cpp
      ${SVS_C_API_DIR}/src/index_vamana.cpp
      ${SVS_C_API_DIR}/src/leanvec_training_data.cpp
      ${SVS_C_API_DIR}/src/data_builder.cpp)
  valkey_search_add_static_library(svs_c_api_static "${SRCS_SVS_C_API}")
  target_include_directories(svs_c_api_static PUBLIC
      ${SVS_C_API_INCLUDE_DIR}
      ${CMAKE_CURRENT_BINARY_DIR}/svs-c-api-build/include)
  target_include_directories(svs_c_api_static PRIVATE ${SVS_C_API_DIR}/src)
  target_compile_options(svs_c_api_static PRIVATE -fvisibility=hidden)
  # Built without OpenMP: threading comes from the module through the C API's
  # threadpool vtable, so libgomp must not enter DT_NEEDED (ci/check_svs_link.sh).
  get_target_property(_svs_c_api_opts svs_c_api_static COMPILE_OPTIONS)
  list(REMOVE_ITEM _svs_c_api_opts -fopenmp)
  set_target_properties(svs_c_api_static PROPERTIES
      COMPILE_OPTIONS "${_svs_c_api_opts}")
  target_link_libraries(svs_c_api_static PRIVATE
      svs::svs
      svs_compile_options
      svs_x86_options_base)
  target_compile_definitions(svs_c_api_static INTERFACE
      VALKEY_SEARCH_SVS_PREBUILT=0)
  message(STATUS "SVS C API: source (public, no LVQ/LeanVec)")
endif()
