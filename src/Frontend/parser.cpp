#include <iostream>
#include <fstream>
#include "antlr4-runtime.h"
#include "castLexer.h"
#include "castParser.h"
#include "castLower.hpp"

using namespace antlr4;

int main(int argc, const char *argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <input-file>" << std::endl;
        return 1;
    }

    std::ifstream stream;
    stream.open(argv[1]);

    if (!stream.is_open())
    {
        std::cerr << "Error: Could not open file " << argv[1] << std::endl;
        return 1;
    }

    ANTLRInputStream input(stream);
    castLexer lexer(&input);
    CommonTokenStream tokens(&lexer);
    castParser parser(&tokens);
    tree::ParseTree *tree = parser.startRule();

    std::string indentedTree = antlr4::tree::Trees::toStringTree(tree, &parser, true);
    std::cout << indentedTree << std::endl;

    mlir::MLIRContext mlirContext;
    LowerVisitor visitor(&mlirContext);

    // test walk tree
    std::any result = visitor.visit(tree);

    return 0;
}