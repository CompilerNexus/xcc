// Emit code

#pragma once

#include <stdint.h>  // int64_t

typedef struct Vector Vector;

void emit_code(Vector *decls);

char *im(int64_t x);
char *immediate_offset(int offset, const char *reg);
char *label_offset_hi(char *label);
char *label_offset_lo(char *label);
