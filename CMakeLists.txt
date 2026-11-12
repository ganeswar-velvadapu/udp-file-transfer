cmake_minimum_required(VERSION 3.16)
project(reliable_udp_file_transfer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)

add_library(rudp
  src/crc32.cpp
  src/protocol.cpp
  src/udp_socket.cpp
  src/rtt_estimator.cpp
  src/receive_window.cpp
  src/sr_sender.cpp
  src/sr_receiver.cpp
  src/tcp_transfer.cpp)
target_include_directories(rudp PUBLIC include)

foreach(app udp_client udp_server tcp_client tcp_server)
  add_executable(${app} apps/${app}.cpp)
  target_link_libraries(${app} PRIVATE rudp)
endforeach()

enable_testing()
add_executable(unit_tests tests/unit_tests.cpp)
target_link_libraries(unit_tests PRIVATE rudp)
add_test(NAME unit_tests COMMAND unit_tests)
add_test(NAME integration
  COMMAND ${CMAKE_SOURCE_DIR}/tests/integration_test.sh ${CMAKE_BINARY_DIR})
