include_guard(GLOBAL)

# Convert a finalized CMake target's actual translation units into stable,
# repository-relative paths. Product targets may not compile generated,
# external, aliased, or duplicate inputs because those inputs cannot be bound
# by the release source identity.
function(_cuac_normalize_product_sources output target)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "product source target does not exist: ${target}")
  endif()
  get_target_property(raw_sources "${target}" SOURCES)
  if(NOT raw_sources OR raw_sources STREQUAL "raw_sources-NOTFOUND")
    message(FATAL_ERROR "product target has no translation units: ${target}")
  endif()

  get_filename_component(source_root "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
  set(result)
  foreach(source IN LISTS raw_sources)
    if(source MATCHES "\\$<")
      message(FATAL_ERROR
              "product target source uses an unsupported generator expression: ${source}")
    endif()
    get_source_file_property(source_generated "${source}" GENERATED)
    if(source_generated)
      message(FATAL_ERROR "product target source is generated: ${source}")
    endif()
    if(IS_ABSOLUTE "${source}")
      set(source_path "${source}")
    else()
      set(source_path "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    endif()
    get_filename_component(source_path "${source_path}" ABSOLUTE)
    if(NOT EXISTS "${source_path}" OR IS_DIRECTORY "${source_path}")
      message(FATAL_ERROR "product target source is not a regular repository file: ${source}")
    endif()
    get_filename_component(source_realpath "${source_path}" REALPATH)
    file(RELATIVE_PATH lexical_relative "${CMAKE_CURRENT_SOURCE_DIR}" "${source_path}")
    file(RELATIVE_PATH relative "${source_root}" "${source_realpath}")
    string(REPLACE "\\" "/" lexical_relative "${lexical_relative}")
    string(REPLACE "\\" "/" relative "${relative}")
    if(relative STREQUAL ".." OR relative MATCHES "^\\.\\./" OR
       IS_ABSOLUTE "${relative}")
      message(FATAL_ERROR "product target source is outside the repository: ${source}")
    endif()
    if(NOT lexical_relative STREQUAL relative)
      message(FATAL_ERROR "product target source uses a filesystem alias: ${source}")
    endif()
    if(NOT relative MATCHES "^[A-Za-z0-9_./-]+\\.cpp$")
      message(FATAL_ERROR "product target source is not a safe C++ translation unit: ${relative}")
    endif()
    list(FIND result "${relative}" duplicate_index)
    if(NOT duplicate_index EQUAL -1)
      message(FATAL_ERROR "product target source is duplicated: ${relative}")
    endif()
    list(APPEND result "${relative}")
  endforeach()
  list(SORT result)
  set(${output} "${result}" PARENT_SCOPE)
endfunction()

function(_cuac_source_array_json output)
  set(result "[")
  set(separator "")
  foreach(source IN LISTS ARGN)
    string(APPEND result "${separator}\"${source}\"")
    set(separator ",")
  endforeach()
  string(APPEND result "]")
  set(${output} "${result}" PARENT_SCOPE)
endfunction()

# This deferred callback runs after the directory's complete CMakeLists has
# been evaluated. The scheduling call must remain the final command in that
# file so no later deferred target mutation can run after the observation.
function(_cuac_write_scheduled_product_source_record)
  foreach(name IN ITEMS OUTPUT PUBLIC_STATIC PUBLIC_LOADABLE CONTROLLED)
    get_property(value DIRECTORY PROPERTY "CUAC_PRODUCT_SOURCES_${name}")
    if(NOT value)
      message(FATAL_ERROR "product source observation omits ${name}")
    endif()
    set("record_${name}" "${value}")
  endforeach()

  _cuac_normalize_product_sources(
    public_static_sources "${record_PUBLIC_STATIC}")
  _cuac_normalize_product_sources(
    public_loadable_sources "${record_PUBLIC_LOADABLE}")
  _cuac_normalize_product_sources(
    controlled_sources "${record_CONTROLLED}")
  if(NOT public_static_sources STREQUAL public_loadable_sources)
    message(FATAL_ERROR
            "static and loadable public product translation units differ")
  endif()

  _cuac_source_array_json(public_json ${public_loadable_sources})
  _cuac_source_array_json(controlled_json ${controlled_sources})
  file(
    WRITE "${record_OUTPUT}"
    "{\n"
    "  \"controlled_translation_units\": ${controlled_json},\n"
    "  \"public_translation_units\": ${public_json}\n"
    "}\n")
endfunction()

function(cuac_defer_product_source_record)
  set(one_value OUTPUT PUBLIC_STATIC PUBLIC_LOADABLE CONTROLLED)
  cmake_parse_arguments(RECORD "" "${one_value}" "" ${ARGN})
  if(RECORD_UNPARSED_ARGUMENTS OR RECORD_KEYWORDS_MISSING_VALUES)
    message(FATAL_ERROR "product source observation arguments are malformed")
  endif()
  get_property(already_scheduled DIRECTORY PROPERTY CUAC_PRODUCT_SOURCES_SCHEDULED)
  if(already_scheduled)
    message(FATAL_ERROR "product source observation was scheduled more than once")
  endif()
  foreach(name IN LISTS one_value)
    if(NOT RECORD_${name})
      message(FATAL_ERROR "product source observation requires ${name}")
    endif()
    set_property(
      DIRECTORY PROPERTY "CUAC_PRODUCT_SOURCES_${name}" "${RECORD_${name}}")
  endforeach()
  set_property(DIRECTORY PROPERTY CUAC_PRODUCT_SOURCES_SCHEDULED TRUE)
  cmake_language(DEFER CALL _cuac_write_scheduled_product_source_record)
endfunction()
