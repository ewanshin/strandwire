add_executable(NetworkClient main.cpp client_session.cpp)
target_link_libraries(NetworkClient PRIVATE netsys_common ProtoLib LogLib)
