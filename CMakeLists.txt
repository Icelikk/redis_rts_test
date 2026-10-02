cmake_minimum_required(VERSION 3.16)
project(redis_rts_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(redis++ CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(loader src/loader.cpp)
target_link_libraries(loader PRIVATE redis++::redis++ Threads::Threads)

add_executable(worker src/worker.cpp)
target_link_libraries(worker PRIVATE redis++::redis++ Threads::Threads)