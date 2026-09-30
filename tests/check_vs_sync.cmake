# Fails when a hand-maintained Visual Studio project lists different sources than its CMake target.
# Invoked by CTest: cmake -DMANIFEST=<file> -DROOT=<repo root> -P check_vs_sync.cmake
# The manifest is written by tests/CMakeLists.txt at configure time and defines
#   VS_SYNC_TARGETS, VS_SYNC_PROJECT_<target>, VS_SYNC_SOURCES_<target>.

include("${MANIFEST}")

set(failed FALSE)
foreach(tgt IN LISTS VS_SYNC_TARGETS)
  set(proj "${ROOT}/${VS_SYNC_PROJECT_${tgt}}")
  if(NOT EXISTS "${proj}")
    message(SEND_ERROR "[${tgt}] missing Visual Studio project: ${VS_SYNC_PROJECT_${tgt}}")
    set(failed TRUE)
    continue()
  endif()

  # Which vcxproj item type to compare. ClCompile by default; ProtoLib uses CustomBuild (.proto).
  set(tag "ClCompile")
  if(DEFINED VS_SYNC_TAG_${tgt})
    set(tag "${VS_SYNC_TAG_${tgt}}")
  endif()

  file(READ "${proj}" content)
  string(REGEX MATCHALL "<${tag} Include=\"[^\"]+\"" matches "${content}")
  set(vs_sources "")
  foreach(m IN LISTS matches)
    string(REGEX REPLACE "<${tag} Include=\"([^\"]+)\"" "\\1" f "${m}")
    list(APPEND vs_sources "${f}")
  endforeach()

  set(cmake_sources ${VS_SYNC_SOURCES_${tgt}})
  list(SORT vs_sources)
  list(SORT cmake_sources)

  if("${vs_sources}" STREQUAL "${cmake_sources}")
    message(STATUS "[${tgt}] in sync: ${cmake_sources}")
  else()
    message(SEND_ERROR "[${tgt}] source lists differ\n  CMake : ${cmake_sources}\n  vcxproj: ${vs_sources}\n  Fix ${VS_SYNC_PROJECT_${tgt}} (and its .filters) or the CMakeLists.txt.")
    set(failed TRUE)
  endif()
endforeach()

if(failed)
  message(FATAL_ERROR "Visual Studio projects are out of sync with CMake targets")
endif()
