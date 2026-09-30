# Static library holding the code generated from the .proto files in this folder.
# Generated sources live in the build tree and are never committed.
set(NETSYS_PROTO_FILES
    chat.proto
    error_code.proto
)

set(_gen_dir ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${_gen_dir})

set(_gen_sources "")
foreach(_proto IN LISTS NETSYS_PROTO_FILES)
  get_filename_component(_name ${_proto} NAME_WE)
  set(_cc ${_gen_dir}/${_name}.pb.cc)
  set(_h ${_gen_dir}/${_name}.pb.h)
  add_custom_command(
    OUTPUT ${_cc} ${_h}
    COMMAND protobuf::protoc
            --cpp_out=${_gen_dir}
            -I ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/${_proto}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_proto} protobuf::protoc
    COMMENT "protoc ${_proto}"
    VERBATIM)
  list(APPEND _gen_sources ${_cc} ${_h})
endforeach()

add_library(ProtoLib STATIC ${_gen_sources} ${NETSYS_PROTO_FILES})
# Include generated headers as "chat.pb.h". SYSTEM keeps generated-code warnings out of /W4 consumers.
target_include_directories(ProtoLib SYSTEM PUBLIC ${_gen_dir})
target_link_libraries(ProtoLib PUBLIC protobuf::libprotobuf)
if(MSVC)
  target_compile_options(ProtoLib PRIVATE /w /utf-8)
else()
  target_compile_options(ProtoLib PRIVATE -w)
endif()

# Read by tests/CMakeLists.txt: check_vs_sync compares this list with ProtoLib.vcxproj.
set_target_properties(ProtoLib PROPERTIES NETSYS_PROTO_FILES "${NETSYS_PROTO_FILES}")
