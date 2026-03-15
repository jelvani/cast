cmake_minimum_required(VERSION 3.14)

set(CMAKE_CXX_COMPILER "clang++")

project(BoostParserExample VERSION 1.0 LANGUAGES CXX)

# boost::parser requires C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find Boost. Note: Adjust this if your Boost installation isn't in a standard system path.
# Since boost::parser is header-only, we just need the headers.
find_package(Boost REQUIRED)

# Define your executable
add_executable(parser_app 
    src/Frontend/parser.cpp
)

# Link Boost headers to your executable
target_link_libraries(parser_app PRIVATE Boost::headers)

# Optional: Enable warnings to catch issues early
if(MSVC)
    target_compile_options(parser_app PRIVATE /W4)
else()
    target_compile_options(parser_app PRIVATE -Wall -Wextra -Wpedantic)
endif()