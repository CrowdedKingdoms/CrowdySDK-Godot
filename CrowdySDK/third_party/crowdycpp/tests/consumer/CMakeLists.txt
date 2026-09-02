cmake_minimum_required(VERSION 3.22)
project(CrowdyCPPConsumer LANGUAGES CXX)

find_package(CrowdyCPP 0.26 CONFIG REQUIRED)

add_executable(crowdycpp_consumer main.cpp)
target_link_libraries(crowdycpp_consumer PRIVATE CrowdyCPP::crowdy)
target_compile_features(crowdycpp_consumer PRIVATE cxx_std_20)
