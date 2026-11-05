cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0167 NEW)
project(market-data-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Boost REQUIRED COMPONENTS system json unit_test_framework)
find_package(OpenSSL REQUIRED)

# --- shared library (non-main sources) ---

add_library(engine_core STATIC
    src/session.cpp
    src/util.cpp
    src/depth_update.cpp
    src/snapshot.cpp
    src/order_book.cpp
    src/http_client.cpp
)

target_include_directories(engine_core PUBLIC src)

target_link_libraries(engine_core
    PUBLIC
        Boost::system
        Boost::json
        OpenSSL::SSL
        OpenSSL::Crypto
)

# --- main executable ---

add_executable(market-data-engine src/main.cpp)
target_link_libraries(market-data-engine PRIVATE engine_core)

# --- tests ---

enable_testing()

add_executable(tests tests/test_main.cpp)

target_link_libraries(tests
    PRIVATE
        engine_core
        Boost::unit_test_framework
)

add_test(NAME unit_tests COMMAND tests)
