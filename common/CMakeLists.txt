# Header-only code shared by the server, the clients and the tests.
# common/lpn/ includes protobuf headers (msgid.h, dispatcher.h), hence the protobuf link.
add_library(netsys_common INTERFACE)
target_include_directories(netsys_common INTERFACE ${CMAKE_SOURCE_DIR})
target_link_libraries(netsys_common INTERFACE asio netsys_options protobuf::libprotobuf)
