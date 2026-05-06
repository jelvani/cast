# cast

cast is a language for describing hardware state machines. A cast program defines
one or more *machines* — finite state automata with typed registers, channel-based
I/O, and explicit control flow — and compiles them to synthesisable SystemVerilog via
MLIR and the CIRCT compiler infrastructure.

```
cast source (.cast)
       │
       ▼
  ANTLR4 parser
       │  AST
       ▼
  MLIR/CIRCT lowering
  (ESI channels → HW/Seq/SV dialects)
       │  MLIR module
       ▼
  exportVerilog
       │  SystemVerilog (.sv)
       ▼
  iverilog / synthesis tool
```

## Table of contents

1. [Prerequisites](#prerequisites)
2. [Building](#building)
3. [Quick start](#quick-start)
4. [Compiler flags](#compiler-flags)
5. [simulate.sh](#simulatesh)
6. [Language reference](#language-reference)
7. [Examples](#examples)
8. [Testing](#testing)
9. [How it works](#how-it-works)

---

## Prerequisites

| Tool | Purpose | Install |
|------|---------|---------|
| clang++ (≥ 17) | C++23 compiler | `brew install llvm` |
| CMake ≥ 3.14, Ninja | Build system | `brew install cmake ninja` |
| LLVM + MLIR | IR infrastructure | build from source (see below) |
| CIRCT | HW/Seq/SV dialects | build from source (see below) |
| ANTLR4 runtime (≥ 4.13) | Parser library | `brew install antlr4-cpp-runtime` |
| ANTLR4 tool jar | Grammar compiler | download `antlr-4.13.2-complete.jar` |
| Java (≥ 11) | Runs ANTLR tool | `brew install openjdk` |
| iverilog | Simulation | `brew install icarus-verilog` |
| gtkwave *(optional)* | Waveform viewer | `brew install gtkwave` |

### Building LLVM + MLIR

```bash
git clone https://github.com/llvm/llvm-project
cd llvm-project
mkdir build && cd build
cmake -G Ninja ../llvm \
  -DLLVM_ENABLE_PROJECTS="mlir" \
  -DLLVM_TARGETS_TO_BUILD="host" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DLLVM_ENABLE_LLD=ON \
  -DLLVM_CCACHE_BUILD=ON
ninja
```

### Building CIRCT

```bash
git clone https://github.com/llvm/circt
cd circt
git submodule init && git submodule update
mkdir build && cd build
cmake -G Ninja .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DMLIR_DIR=/path/to/llvm-project/build/lib/cmake/mlir \
  -DLLVM_DIR=/path/to/llvm-project/build/lib/cmake/llvm \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DLLVM_ENABLE_LLD=ON
ninja
```

---

## Building

```bash
git clone <this-repo> cast && cd cast
mkdir build && cd build
cmake .. -G Ninja \
  -DCMAKE_PREFIX_PATH="/path/to/llvm-project/build;/path/to/circt/build"
ninja
```

The build produces a single binary: `build/castc`.

---

## Quick start

Compile and simulate any `.cast` file in one command:

```bash
./simulate.sh examples/counter.cast
```

Expected output:

```
==> compiling examples/counter.cast
    verilog written to /tmp/counter.sv
==> elaborating
==> simulating
count: 0
count: 1
count: 2
...
count: 9
--- wrap: resetting to 0 ---
count: 0
...
```

---

## Compiler flags

```
castc [options] <file.cast>
```

| Flag | Default | Description |
|------|---------|-------------|
| `--tb` | off | Append an embedded `module tb` testbench to the output |
| `--duration=<ns>` | `500` | Simulation run length in nanoseconds (used by `--tb`) |
| `--vcd=<path>` | `/tmp/<basename>.vcd` | VCD waveform dump path (used by `--tb`) |

Without `--tb` the output is pure synthesisable SystemVerilog with no simulation
constructs.

### Manual compilation steps

```bash
# 1. Compile to SystemVerilog (with embedded testbench)
./build/castc --tb examples/counter.cast > /tmp/counter.sv

# 2. Elaborate
iverilog -g2012 -gno-assertions -o /tmp/counter_sim /tmp/counter.sv

# 3. Simulate
vvp /tmp/counter_sim
```

### Viewing waveforms

```bash
./build/castc --tb --vcd=/tmp/counter.vcd examples/counter.cast > /tmp/counter.sv
iverilog -g2012 -gno-assertions -o /tmp/counter_sim /tmp/counter.sv
vvp /tmp/counter_sim
gtkwave /tmp/counter.vcd
```

---

## simulate.sh

The helper script wraps the three manual steps above:

```
./simulate.sh <file.cast> [--duration=<ns>] [--vcd=<path>]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--duration=<ns>` | `500` | How long the simulation runs |
| `--vcd=<path>` | `/tmp/<basename>.vcd` | Where the VCD waveform is written |

Options after the file are forwarded directly to `castc`.

---

## Language reference

### Program structure

A cast program contains one or more `machine` definitions followed by a single
`instantiate` block.

```cast
machine <Name> {
    interface { ... }   // I/O channels
    shared    { ... }   // persistent registers
    states    { ... }   // state definitions
}

instantiate {
    x = <Name>();       // create an instance
    x.port <- value;    // feed a channel
}
```

### Machines

A machine is a named state automaton. Each machine compiles to a hardware module
with `clk` and `rst` ports plus one port per declared channel.

### interface

Declares the machine's I/O channels. Every channel has a direction, a type, and a
name.

```cast
interface {
    input  uint16 data_in;
    output bool   ready;
}
```

| Direction | Meaning |
|-----------|---------|
| `input`   | The machine receives values on this channel |
| `output`  | The machine sends values on this channel |

### shared

Declares registers that persist across states. These become synchronous registers
(`seq.compreg`) in the generated Verilog.

```cast
shared {
    var uint32 counter;
    var bool   flag;
}
```

### states

A states block contains one or more named states. The **first declared state** is
always the reset/start state.

```cast
states {
    idle: channel_name -> local_var {   // header: block on channel receive
        // body: runs once per received value
        goto next_state;
    }
    next_state: {
        // body: runs every cycle
        goto idle;
    }
}
```

#### State headers (channel receive)

A state can begin with one or more channel-receive bindings before the body block:

```
state_name: channel -> local_name { ... }
state_name: ch1 -> a, ch2 -> b { ... }
```

The body only executes in cycles where all listed channels have valid data.
`local_name` is a combinational alias for the received value — it is visible to
all statements in the body including `print`.

#### goto

Every execution path through a state body must end with a `goto`:

```cast
goto other_state;
```

### Types

| cast type | Width | Notes |
|-----------|-------|-------|
| `bool`    | 1-bit | |
| `byte`    | 8-bit | unsigned |
| `uint16`  | 16-bit | unsigned |
| `uint32`  | 32-bit | unsigned |
| `int32`   | 32-bit | signed |
| `int`     | 32-bit | signed |

### Operators

| Category | Operators |
|----------|-----------|
| Arithmetic | `+` `-` `*` `/` `%` |
| Bitwise | `&` `\|` `^` `<<` `>>` |
| Comparison | `==` `!=` `<` `<=` `>` `>=` |
| Logical | `&&` `\|\|` `!` |
| Compound assign | `+=` `-=` `*=` `/=` `^=` `<<=` `>>=` |
| Increment/decrement | `++` `--` |

### Channel operations

| Syntax | Meaning |
|--------|---------|
| `ch -> v` | Receive from channel `ch` into local binding `v` (state header) |
| `ch <- expr` | Send `expr` on output channel `ch` |
| `inst.port <- expr` | Feed a value into instance `inst`'s input channel |

### Control flow

```cast
if expr { ... }
if expr { ... } else { ... }
if expr { ... } else if expr { ... } else { ... }

for (var uint16 i = 0; i < 10; i++) { ... }

goto state_name;
```

### print

`print` lowers to `$display` in simulation (no-op in synthesis):

```cast
print("label: ", value);
print("a=", a, " b=", b);
```

### instantiate

The `instantiate` block creates machine instances and feeds their input channels.
Feeds become preloaded FIFO register values — the machine sees them on its first
cycle.

```cast
instantiate {
    m = MyMachine();
    m.data <- 42;
}
```

---

## Examples

| File | Level | Description |
|------|-------|-------------|
| [counter.cast](examples/counter.cast) | Beginner | Counts 0–9 and wraps — the simplest self-contained state machine |
| [fibonacci.cast](examples/fibonacci.cast) | Beginner | Generates the Fibonacci sequence using two shared registers |
| [gcd.cast](examples/gcd.cast) | Beginner | GCD(48, 18) via repeated subtraction (Euclidean algorithm) |
| [alarm.cast](examples/alarm.cast) | Intermediate | Threshold alarm with edge detection — prints only on level change |
| [running_average.cast](examples/running_average.cast) | Intermediate | Accumulates 8 samples and prints their integer average |
| [traffic_light.cast](examples/traffic_light.cast) | Intermediate | Red/green/yellow cycle with per-phase countdown timers |
| [debounce.cast](examples/debounce.cast) | Intermediate | Counter-based digital debouncer — requires 16 stable cycles to latch |
| [checksum.cast](examples/checksum.cast) | Intermediate | Rolling XOR checksum over a 5-byte window |
| [pwm.cast](examples/pwm.cast) | Intermediate | Pulse-width modulator — duty cycle set via input channel |
| [shift_register.cast](examples/shift_register.cast) | Advanced | Serial-in parallel-out: assembles 8 bits into a byte |
| [binary_search.cast](examples/binary_search.cast) | Advanced | Binary search on a [0, 127] range with multi-cycle halving |
| [popcount.cast](examples/popcount.cast) | Advanced | Counts set bits in a 16-bit word (LSB-first iteration) |
| [uart_tx.cast](examples/uart_tx.cast) | Advanced | 8N1 UART transmitter — serialises a byte with start/stop framing |

Run any example:

```bash
./simulate.sh examples/fibonacci.cast
./simulate.sh examples/uart_tx.cast
```

---

## Testing

`ninja check` compiles every listed example through two stages and reports results:

```bash
ninja -C build check
```

| Stage | Tool | What it checks |
|-------|------|----------------|
| 1 | `castc --tb` | cast source compiles without error |
| 2 | `iverilog -g2012 -gno-assertions` | generated Verilog is valid and elaborates cleanly |

Output on a passing run:

```
Test project /path/to/cast/build
    Start  1: example.simple
1/14 Test  #1: example.simple ...........   Passed
    Start  2: example.counter
2/14 Test  #2: example.counter ..........   Passed
...
14/14 Test #14: example.uart_tx ..........  Passed

100% tests passed, 0 tests failed out of 14
```

On failure the stderr from the failing stage is shown. To run a single test:

```bash
ctest --test-dir build -R example.fibonacci --output-on-failure
```

### Adding a new example to the test suite

Place the file in `examples/` and add one line to `CMakeLists.txt`:

```cmake
add_cast_test(my_new_example)
```

---

## How it works

### Frontend

The ANTLR4 grammar ([cast.g4](src/Frontend/antlr/cast.g4)) parses cast source into
a concrete syntax tree. The visitor in [castLower.cpp](src/Frontend/castLower.cpp)
walks the CST and emits MLIR operations directly, with no intermediate AST.

### MLIR/CIRCT lowering pipeline

Each machine becomes an `esi.pure_module` containing:

- **ESI channels** (`esi.channel<T>`) for every declared interface port
- **`seq.compreg`** for every `shared` variable and FIFO stub register
- **`hw.module`** with `clk` and `rst` ports generated by the ESI lowering passes

The passes run in this order:

```
ESIPhysical → ESIBundle → ESIPort → ESIType →
ESItoHW → SeqFIFO → SeqHLMem → VerifToSV →
LowerSeqToSV → exportVerilog
```

### Testbench generation

When `--tb` is passed, `castc` appends a `module tb` after `exportVerilog` output.
The testbench instantiates the generated `Main` module, drives `clk` and `rst`,
opens a VCD dump, and calls `$finish` after `--duration` nanoseconds.
