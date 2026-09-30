add_executable(wire_test wire_test.cpp)
target_link_libraries(wire_test PRIVATE netsys_common ProtoLib)
add_test(NAME wire_test COMMAND wire_test)

add_executable(smoke_test smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE ServerLib)
add_test(NAME smoke_test COMMAND smoke_test)
set_tests_properties(smoke_test PROPERTIES TIMEOUT 60)

add_executable(app_test app_test.cpp)
target_link_libraries(app_test PRIVATE ServerLib)
add_test(NAME app_test COMMAND app_test)
set_tests_properties(app_test PROPERTIES TIMEOUT 60)

# The Visual Studio solution (strandwire.sln) is maintained by hand next to these CMake
# targets. This test fails when a vcxproj and its CMake target list different sources.
set(_vs_sync_manifest "")
foreach(_pair IN ITEMS
    "LogLib=LogLib/LogLib.vcxproj"
    "ServerLib=ServerLib/ServerLib.vcxproj"
    "NetworkServer=NetworkServer/NetworkServer.vcxproj"
    "NetworkClient=NetworkClient/NetworkClient.vcxproj"
    "DummyClient=DummyClient/DummyClient.vcxproj"
    "wire_test=tests/wire_test.vcxproj"
    "smoke_test=tests/smoke_test.vcxproj"
    "app_test=tests/app_test.vcxproj")
  string(REPLACE "=" ";" _parts "${_pair}")
  list(GET _parts 0 _tgt)
  list(GET _parts 1 _proj)
  get_target_property(_srcs ${_tgt} SOURCES)
  string(APPEND _vs_sync_manifest
    "list(APPEND VS_SYNC_TARGETS ${_tgt})\n"
    "set(VS_SYNC_PROJECT_${_tgt} \"${_proj}\")\n"
    "set(VS_SYNC_SOURCES_${_tgt} \"${_srcs}\")\n")
endforeach()

# ProtoLib compiles generated code, so its source lists cannot match. Compare the .proto lists
# instead: CMake's NETSYS_PROTO_FILES against the vcxproj's <CustomBuild> items.
get_target_property(_protos ProtoLib NETSYS_PROTO_FILES)
string(APPEND _vs_sync_manifest
  "list(APPEND VS_SYNC_TARGETS ProtoLib)\n"
  "set(VS_SYNC_PROJECT_ProtoLib \"ProtoLib/ProtoLib.vcxproj\")\n"
  "set(VS_SYNC_SOURCES_ProtoLib \"${_protos}\")\n"
  "set(VS_SYNC_TAG_ProtoLib \"CustomBuild\")\n")

file(WRITE ${CMAKE_BINARY_DIR}/vs_sync_manifest.cmake "${_vs_sync_manifest}")

add_test(NAME check_vs_sync
  COMMAND ${CMAKE_COMMAND}
    -DMANIFEST=${CMAKE_BINARY_DIR}/vs_sync_manifest.cmake
    -DROOT=${CMAKE_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_vs_sync.cmake)
