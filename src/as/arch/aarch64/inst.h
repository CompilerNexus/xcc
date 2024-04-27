// aarch64 Instruction

#pragma once

#include <stdint.h>  // int64_t

typedef struct Expr Expr;

// Must match the order with kOpTable in parse_aarch64.c
enum Opcode {
  NOOP,
  MOV,
  RET,
};

enum RegType {
  NOREG = -1,

  // 32bit
   W0,  W1,  W2,  W3,  W4,  W5,  W6,  W7,  W8,  W9, W10, W11, W12, W13, W14, W15,
  W16, W17, W18, W19, W20, W21, W22, W23, W24, W25, W26, W27, W28, W29, W30, W31,

  // 64bit
   X0,  X1,  X2,  X3,  X4,  X5,  X6,  X7,  X8,  X9, X10, X11, X12, X13, X14, X15,
  X16, X17, X18, X19, X20, X21, X22, X23, X24, X25, X26, X27, X28, //X29, X30, X31,
  FP, LR, SP,
};

enum RegSize {
  REG32,
  REG64,
};

typedef struct {
  char size;  // RegSize
  char no;  // 0~31
} Reg;

enum OperandType {
  NOOPERAND,
  REG,        // reg
  IMMEDIATE,  // 1234
};

typedef struct {
  enum OperandType type;
  union {
    Reg reg;
    int64_t immediate;
  };
} Operand;

typedef struct Inst {
  enum Opcode op;
  Operand opr1;
  Operand opr2;
  Operand opr3;
} Inst;
