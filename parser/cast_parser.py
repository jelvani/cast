from lark import Lark
from pathlib import Path
import argparse
_Parser__BASE_DIR = Path(__file__).resolve().parent

class Parser:
    def __init__(self, file,debug=False):
        with open(_Parser__BASE_DIR / "cast.lark", "r", encoding="utf-8") as f:
            self.grammar = f.read()
        
        self.parser = Lark(self.grammar,start="start", parser="earley",debug=debug)
        self.parse_tree = self.parse(file)

    def parse(self, file):
        with open(file, "r", encoding="utf-8") as f:
            prog = f.read()

        return self.parser.parse(prog)
    
    def __str__(self):
        return self.parse_tree.pretty()





def main():
    parser = argparse.ArgumentParser(
        description="Parse a cast program."
    )
    parser.add_argument(
        "cast_file",
        type=str,
        help="Path to the .cast file to parse"
    )

    args = parser.parse_args()

    cast_parser = Parser(args.cast_file)
    print(cast_parser)

if __name__ == "__main__":
    main()