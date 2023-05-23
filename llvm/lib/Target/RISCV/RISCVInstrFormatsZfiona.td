//===-- RISCVInstrFormatsZfiona.td - RISC-V V Instruction Formats -*- tablegen -*-=//
//===----------------------------------------------------------------------===//
//
// This file describes the Fiona extension instruction formats.
// 
//===----------------------------------------------------------------------===//


// Fiona extension instruction formats

// The following opcode name match the "custom-0" that given in Table 19.1 in the
// RISC-V User-level ISA specification ("RISC-V base opcode map").
def FIONA_OPCODE : RISCVOpcode<"FIONA_OP", 0b0001011>;

class RVInstFiona<bits<3> funct3, bits<7> funct7,
               dag outs, dag ins, string opcodestr, string argstr>
    : RVInst<outs, ins, opcodestr, argstr, [], InstFormatOther> {

  bits<5> rs2;
  bits<5> rs1;
  bits<5> rd;

  let Inst{31-25} = funct7;
  let Inst{24-20} = rs2;
  let Inst{19-15} = rs1;
  let Inst{14-12} = funct3;
  let Inst{11-7}  = rd;
  let Opcode = FIONA_OPCODE.Value;
}

class RVInstFiona_ConstantRS2<bits<3> funct3, bits<7> funct7, bits<5> rs2,
               dag outs, dag ins, string opcodestr, string argstr>
    : RVInst<outs, ins, opcodestr, argstr, [], InstFormatOther> {

  bits<5> rs1;
  bits<5> rd;

  let Inst{31-25} = funct7;
  let Inst{24-20} = rs2;
  let Inst{19-15} = rs1;
  let Inst{14-12} = funct3;
  let Inst{11-7}  = rd;
  let Opcode = FIONA_OPCODE.Value;
}

class RVInstFiona_ConstantRD<bits<3> funct3, bits<7> funct7, bits<5> rd,
               dag outs, dag ins, string opcodestr, string argstr>
    : RVInst<outs, ins, opcodestr, argstr, [], InstFormatOther> {
  bits<5> rs2;
  bits<5> rs1;

  let Inst{31-25} = funct7;
  let Inst{24-20} = rs2;
  let Inst{19-15} = rs1;
  let Inst{14-12} = funct3;
  let Inst{11-7}  = rd;
  let Opcode = FIONA_OPCODE.Value;
}