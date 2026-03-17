cmake_minimum_required(VERSION 3.14)
include(CMakePrintHelpers) # for debugging

set(CMAKE_CXX_COMPILER "clang++")

project(CastCompiler VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LLVM REQUIRED CONFIG)
find_package(MLIR REQUIRED CONFIG)

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}")
include(AddLLVM)
include(AddMLIR)

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
include_directories(SYSTEM ${MLIR_INCLUDE_DIRS})

set(GRAMMAR_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/Frontend/antlr/cast.g4)

set(ANTLR_GEN_DIR ${CMAKE_BINARY_DIR}/generated)

set(ANTLR_EXECUTABLE "antlr-4.13.2-complete.jar")

set(ANTLR_OUTPUTS
    ${ANTLR_GEN_DIR}/castLexer.cpp
    ${ANTLR_GEN_DIR}/castLexer.h
    ${ANTLR_GEN_DIR}/castParser.cpp
    ${ANTLR_GEN_DIR}/castParser.h
    ${ANTLR_GEN_DIR}/castVisitor.cpp
    ${ANTLR_GEN_DIR}/castVisitor.h
    ${ANTLR_GEN_DIR}/castBaseVisitor.cpp
    ${ANTLR_GEN_DIR}/castBaseVisitor.h
)

add_custom_command(
    OUTPUT ${ANTLR_OUTPUTS}
    COMMAND ${ANTLR_EXECUTABLE} -Dlanguage=Cpp -visitor -no-listener -o ${ANTLR_GEN_DIR} ${GRAMMAR_FILE}
    DEPENDS ${GRAMMAR_FILE}
    COMMENT "Generating ANTLR4 C++ sources for cast.g4"
)

find_library(ANTLR4_LIB antlr4-runtime PATHS /usr/local/lib)
include_directories(/usr/local/include/antlr4-runtime)

add_executable(castc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Frontend/parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Frontend/castLower.cpp
    ${ANTLR_OUTPUTS}
)

target_include_directories(castc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ANTLR_GEN_DIR}
)

target_link_libraries(castc
    ${ANTLR4_LIB}
    MLIRIR
)

target_compile_options(castc PRIVATE -Wall -Wextra -Wpedantic)
