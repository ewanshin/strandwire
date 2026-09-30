add_library(ServerLib STATIC
    session_manager.cpp
    session.cpp
    lobby_service.cpp
    server.cpp
)
target_link_libraries(ServerLib PUBLIC netsys_common ProtoLib LogLib)
