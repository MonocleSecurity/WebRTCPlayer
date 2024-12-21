cmake_minimum_required(VERSION 3.21)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake CACHE STRING "Vcpkg toolchain file")

project(WebRTCPlayer C CXX)

find_package(boost_scope_exit CONFIG REQUIRED)
find_package(boost_optional CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)
find_package(LibDataChannel CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(WebRTCPlayer
main.cpp)

set_property(TARGET WebRTCPlayer PROPERTY CXX_STANDARD 17)

target_include_directories(WebRTCPlayer PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(WebRTCPlayer PRIVATE ${FFMPEG_LIBRARY_DIRS})

target_link_libraries(WebRTCPlayer ${FFMPEG_LIBRARIES})
target_link_libraries(WebRTCPlayer Boost::optional)
target_link_libraries(WebRTCPlayer Boost::scope_exit)
target_link_libraries(WebRTCPlayer httplib::httplib)
target_link_libraries(WebRTCPlayer LibDataChannel::LibDataChannel)
target_link_libraries(WebRTCPlayer nlohmann_json::nlohmann_json)

if(WIN32)
target_link_libraries(WebRTCPlayer Ws2_32.lib)
endif()
