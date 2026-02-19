# Running the Parser
Make sure to have `lark` python package installed.
```bash
python3 parser/cast_parser.py fft.cast
```


# Building MLIR CIRCT for compiler (NOT DONE YET)


First build llvm as a submodule
```bash
git clone https://github.com/llvm/circt
cd circt
git submodule init
git submodule update

mkdir llvm/build
cd llvm/build

python3 -m venv ~/circt-venv
source ~/circt-venv/bin/activate

cmake -G Ninja ../llvm -DLLVM_ENABLE_PROJECTS="mlir" -DLLVM_TARGETS_TO_BUILD="host" -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DLLVM_ENABLE_LLD=ON -DLLVM_CCACHE_BUILD=ON -DMLIR_ENABLE_BINDINGS_PYTHON=ON

ninja
ninja check-mlir
```

Now build circt

```bash
cd circt
mkdir build
cd build

cmake -G Ninja ../llvm/llvm -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS=mlir -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_EXTERNAL_PROJECTS=circt -DLLVM_EXTERNAL_CIRCT_SOURCE_DIR=.. -DMLIR_ENABLE_BINDINGS_PYTHON=ON -DCIRCT_BINDINGS_PYTHON_ENABLED=ON -DLLVM_CCACHE_BUILD=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DLLVM_ENABLE_LLD=ON

ninja

export PYTHONPATH="$PWD/llvm/build/tools/circt/python_packages/circt_core"
```

# Lowering MLIR to Verilog

```bash
circt-opt --export-verilog file.mlir
```