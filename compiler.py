import circt
from circt.ir import Context, InsertionPoint, IntegerType, Location, Module
from circt.dialects import hw, comb
from parser import cast_parser
from lark.visitors import Visitor



parser = cast_parser.Parser("simple.cast")


class Lower(Visitor):
    def decl_machine(self, tree):
        print(tree.pretty())
        d = tree.find_data("decl_machine")
        for data in d:
            print(data.data)




Lower().visit(parser.parse_tree)