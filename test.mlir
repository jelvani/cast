module {
  hw.module @magic(in %a : i42, in %b : i42, out c : i42) {
    %0 = comb.xor %a, %b : i42
    hw.output %0 : i42
  }
}