//===-- RISCVInstrInfoZfiona.td - RISC-V Optical Accelerator instructions -*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Instruction formats
//===----------------------------------------------------------------------===//
include "RISCVInstrFormatsZfiona.td"

class RVInstFiona_VVS<bits<3> funct3, bits<7> funct7, string opcodestr> : RVInstFiona<funct3,
    funct7, (outs GPR:$rd), (ins FIOVReg:$rs1, FIOVReg:$rs2),
    opcodestr, "$rd, $rs1, $rs2">;

class RVInstFiona_UVV<bits<3> funct3, bits<7> funct7, string opcodestr> : RVInstFiona<funct3,
    funct7, (outs FIOVReg:$rd), (ins FIOVReg:$rs1),
    opcodestr, "$rd, $rs1"> {
        let rs2 = 0;
    }

class RVInstFiona_VVV<bits<3> funct3, bits<7> funct7, string opcodestr> : RVInstFiona<funct3,
    funct7, (outs FIOVReg:$rd), (ins FIOVReg:$rs1, FIOVReg:$rs2),
    opcodestr, "$rd, $rs1, $rs2">;

class RVInstFiona_SVV<bits<3> funct3, bits<7> funct7, string opcodestr> : RVInstFiona<funct3,
    funct7, (outs FIOVReg:$rd), (ins FIOVReg:$rs1, GPR:$rs2),
    opcodestr, "$rd, $rs1, $rs2">;

class RVInstFiona_CVS<bits<3> funct3, bits<7> funct7, bits<5> rs2, string opcodestr> : RVInstFiona_ConstantRS2<funct3,
    funct7, rs2, (outs GPR:$rd), (ins FIOVReg:$rs1),
    opcodestr, "$rd, $rs1">;

class RVInstFiona_CVV<bits<3> funct3, bits<7> funct7, bits<5> rs2, string opcodestr> : RVInstFiona_ConstantRS2<funct3,
    funct7, rs2, (outs FIOVReg:$rd), (ins FIOVReg:$rs1),
    opcodestr, "$rd, $rs1">;

class RVInstFiona_USV<bits<3> funct3, bits<7> funct7, string opcodestr> : RVInstFiona<funct3,
    funct7, (outs FIOVReg:$rd), (ins GPR:$rs1),
    opcodestr, "$rd, $rs1"> {
        let rs2 = 0;
    }

class RVInstFiona_VSU<bits<3> funct3, bits<7> funct7, string opcodestr> : RVInstFiona<funct3,
    funct7, (outs), (ins GPR:$rs1, FIOVReg:$rs2),
    opcodestr, "$rs1, $rs2"> {
        let rd = 0;
    }

class RVInstFiona_USU<bits<3> funct3, bits<7> funct7, string opcodestr> : RVInstFiona<funct3,
    funct7, (outs), (ins GPR:$rs1),
    opcodestr, "$rs1"> {
        let rd = 0;
        let rs2 = 0;
    }

class RVInstFiona_SSU<bits<3> funct3, bits<7> funct7, string opcodestr> : RVInstFiona<funct3,
    funct7, (outs), (ins GPR:$rs1, GPR:$rs2),
    opcodestr, "$rs1, $rs2"> {
        let rd = 0;
    }

class RVInstFiona_USC<bits<3> funct3, bits<7> funct7, bits<5> rd, string opcodestr> : RVInstFiona_ConstantRD<funct3,
    funct7, rd, (outs), (ins GPR:$rs1),
    opcodestr, "$rs1"> {
        let rs2 = 0;
    }

class RVInstFiona_SSC<bits<3> funct3, bits<7> funct7, bits<5> rd, string opcodestr> : RVInstFiona_ConstantRD<funct3,
    funct7, rd, (outs), (ins GPR:$rs1, GPR:$rs2),
    opcodestr, "$rs1, $rs2">;


let Predicates = [HasCustomFiona] in {
let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in {
def DOTPROD     : RVInstFiona_VVS<0b100, 0b1000001, "dotprod">;
def MATMUL      : RVInstFiona_UVV<0b000, 0b1000010, "matmul">;
def CONV1D      : RVInstFiona_VVS<0b100, 0b1000011, "conv1d">;
def ADD_V       : RVInstFiona_VVV<0b000, 0b0000001, "add.v">;
def SUB_V       : RVInstFiona_VVV<0b000, 0b0000010, "sub.v">;
def ADD_VS      : RVInstFiona_SVV<0b001, 0b0000011, "add.vs">;
def SUB_VS      : RVInstFiona_SVV<0b001, 0b0000100, "sub.vs">;
def MUL_VS      : RVInstFiona_SVV<0b001, 0b0000101, "mul.vs">;
def DIV_VS      : RVInstFiona_SVV<0b001, 0b0000110, "div.vs">;
def SHUFFLE_V   : RVInstFiona_VVV<0b000, 0b0001010, "shuffle.v">;
def MAX_V       : RVInstFiona_CVS<0b100, 0b0001011, 0b00000, "max.v">;
def MIN_V       : RVInstFiona_CVS<0b100, 0b0001011, 0b00001, "min.v">;
def PRELU_V     : RVInstFiona_SVV<0b001, 0b0001111, "prelu.v">;
def TANH_V      : RVInstFiona_CVV<0b000, 0b0001111, 0b00001, "tanh.v">;
def SIGMOID_V   : RVInstFiona_CVV<0b000, 0b0001111, 0b00010, "sigmoid.v">;
def LOAD_V      : RVInstFiona_USV<0b010, 0b0010000, "load.v">;
def STORE_V     : RVInstFiona_VSU<0b010, 0b0010001, "store.v">;
def SET_STRIDE  : RVInstFiona_USC<0b010, 0b0011000, 0b00000, "set.stride">;
def SET_VLEN    : RVInstFiona_USC<0b010, 0b0011000, 0b00001, "set.vlen">;
def SET_VMASK   : RVInstFiona_SSC<0b011, 0b0011000, 0b00010, "set.vmask">;
def SET_MAT     : RVInstFiona_SSC<0b011, 0b0011000, 0b00011, "set.mat">;
} // hasSideEffects = 0, mayLoad = 0, mayStore = 0
} //Predicates = [HasCustomFiona]

