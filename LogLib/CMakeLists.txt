# Logger wrapper around spdlog (header-only, compiled only inside logger.cpp).
# Users of LogLib include "LogLib/logger.h" and never see spdlog.
add_library(LogLib STATIC logger.cpp)
target_include_directories(LogLib PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(LogLib SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/spdlog/include)
target_link_libraries(LogLib PUBLIC Threads::Threads PRIVATE netsys_options)
