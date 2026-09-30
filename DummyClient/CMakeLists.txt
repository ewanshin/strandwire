add_executable(DummyClient main.cpp dummy_session.cpp)
target_link_libraries(DummyClient PRIVATE netsys_common ProtoLib LogLib)
