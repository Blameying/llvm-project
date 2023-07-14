; ModuleID = 'test_vector_riscv_unmasked_nxv32i32'

target datalayout = "e-m:e-p:32:32-i64:64-n32-S128"
target triple = "riscv32-unknown-elf"

define void @test_load_vector(<vscale x 32 x i32>* %ptr, <vscale x 32 x i32>* %res) {
entry:
  %load = load <vscale x 32 x i32>, <vscale x 32 x i32>* %ptr
  store <vscale x 32 x i32> %load, <vscale x 32 x i32>* %res
  ret void
}
