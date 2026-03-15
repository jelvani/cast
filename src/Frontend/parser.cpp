#include <iostream>
#include <string>
#include <string_view>
#include <boost/parser/parser.hpp>

namespace bp = boost::parser;

// -------------------------------------------------------------------------
// 1. Skipper (Handles %ignore WS, CPP_LINE_COMMENT, CPP_BLOCK_COMMENT)
// -------------------------------------------------------------------------
auto const line_comment = bp::lit("//") >> *(bp::char_ - '\n') >> ('\n' | bp::eoi);
auto const block_comment = bp::lit("/*") >> *(bp::char_ - bp::lit("*/")) >> bp::lit("*/");
auto const skipper = bp::ws | line_comment | block_comment;

// -------------------------------------------------------------------------
// 2. Rules Declarations
// Boost.Parser uses 'bp::rule<struct Tag, AttributeType>'
// (Attributes omitted here for a structural/validation parser)
// -------------------------------------------------------------------------
bp::rule<struct start_tag> const start = "start";
bp::rule<struct space_decl_tag> const space_decl = "space_decl";
bp::rule<struct imports_tag> const imports = "imports";
bp::rule<struct single_import_tag> const single_import = "single_import";
bp::rule<struct multiple_imports_tag> const multiple_imports = "multiple_imports";

bp::rule<struct decl_package_tag> const decl_package = "decl_package";
bp::rule<struct decl_func_tag> const decl_func = "decl_func";
bp::rule<struct decl_machine_tag> const decl_machine = "decl_machine";
bp::rule<struct decl_type> const decl_type = "decl_type";
bp::rule<struct type_rule_tag> const type_rule = "type_rule";
bp::rule<struct ident_typed_tag> const ident_typed = "ident_typed";

bp::rule<struct stmt_block_tag> const stmt_block = "stmt_block";
bp::rule<struct stmt_tag> const stmt = "stmt";
bp::rule<struct expr_tag> const expr = "expr";

// -------------------------------------------------------------------------
// 3. Tokens and Literals
// -------------------------------------------------------------------------
bp::rule<struct CNAME_tag, std::string> const CNAME = "CNAME";
auto const CNAME_def = bp::lexeme[*(bp::char_('_'))];

auto const NUMBER = bp::int_; // Simplification for integers

bp::rule<struct HEX_tag, std::string> const HEX = "HEX";
auto const HEX_def = bp::lexeme["0x" >> +bp::hex];

bp::rule<struct ESCAPED_STRING_tag, std::string> const ESCAPED_STRING = "ESCAPED_STRING";
auto const ESCAPED_STRING_def = bp::lexeme['"' >> *(bp::char_ - '"') >> '"'];

// -------------------------------------------------------------------------
// 4. Grammar Definitions (Mapping EBNF to Boost.Parser)
// Note: '-' means optional, '%' means separated list
// -------------------------------------------------------------------------

// Package & Space
auto const decl_package_def = bp::lit("package") >> CNAME;

// Imports
auto const single_import_def = -bp::lit("(") >> CNAME >> -bp::lit(")");
auto const multiple_imports_def = bp::lit("(") >> +CNAME >> bp::lit(")");
auto const imports_def = bp::lit("import") >> (single_import | multiple_imports);

// Types (Simplified structural demonstration)
auto const type_lit = bp::lit("string") | "int" | "float" | "byte" | "bool";
auto const bin_op_def = "+" | "-" | "*" | "/" | "&&" | "||" | "!=" | "==" | ">=" | "<=" | ">" | "<" | "%" | "<<" | ">>" | "|" | "&" | "^";
auto const type_rule_def = type_lit /* | type_inline | type_chan */;
auto const ident_typed_def = type_rule >> CNAME >> *(bp::lit(",") >> CNAME);

// Expressions (Refactored to avoid Left-Recursion)
auto const primary_expr = CNAME | NUMBER | HEX | ESCAPED_STRING | (bp::lit("(") >> expr >> bp::lit(")"));
auto const expr_def = primary_expr >> *( (bp::lit("+") | "-" | "*" | "/") >> primary_expr );
auto const expr_field_def = expr >> bp::lit(".") >> expr;
auto const expr_multiple_def = expr >> -(*(bp::lit(",") >> expr));
auto const expr_binary_def = ex-r >> bin_op << expr

// Statements
auto const stmt_def = (
    (bp::lit("nextstate") >> CNAME) |
    (bp::lit("return") >> (expr % ',')) |
    expr // Fallback to expression
) >> bp::lit(";");

auto const states_def = bp::lit("states") >> states_block >> bp::lit(";");

// block defs
auto const states_block_def = bp::lit("{") >> decl_state >> states_block >> bp::lit("}");
auto const stmt_block_def = bp::lit("{") >> *stmt >> bp::lit("}");
auto const machine_block_def = bp::lit("{") >> decl_interface >> -(*decl_state) >> bp::lit("}");

// Functions
auto const func_args = bp::lit("(") >> -(ident_typed % ',') >> bp::lit(")");
auto const decl_func_def = bp::lit("func") >> CNAME >> func_args >> -type_rule >> stmt_block;
auto const decl_machine_def = bp::lit("machine") >> CNAME >> func_args >> machine_block;
auto const decl_state_def = CNAME >> bp::lit(":")

// Top Level Rule
auto const start_def = -decl_package 
                    >> -imports 
                    >> *(decl_func | decl_machine | decl_instantiate);

// -------------------------------------------------------------------------
// 5. Tie Rules to Definitions
// Boost.Parser uses a single macro to bind them all in the namespace scope.
// -------------------------------------------------------------------------
BOOST_PARSER_DEFINE_RULES(
    CNAME, HEX, ESCAPED_STRING,
    start, space_decl, imports, single_import, multiple_imports,
    decl_package, decl_func, type_rule, ident_typed,
    stmt_block, stmt, expr
);


// -------------------------------------------------------------------------
// 6. Driver Function
// -------------------------------------------------------------------------
int main() {
    std::string_view input = R"(
        /* Sample Input */
        package MyPackage
        import ( MyImport )

        func my_function(int my_var) {
            return 0;
        }
    )";

    // Boost.Parser makes the parsing call much simpler than Spirit
    bool success = bp::parse(input, start, skipper);

    if (success) {
        std::cout << "Parsing completed successfully!\n";
    } else {
        std::cout << "Parsing failed due to syntax error.\n";
    }

    return 0;
}