#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "xcc.h"

#define CURIP(ofs)  (start_address + codesize + ofs)
#define ADD_CODE(...)  do { unsigned char buf[] = {__VA_ARGS__}; add_code(buf, sizeof(buf)); } while (0)
#include "x86_64.h"

char *strdup_(const char *str) {
  size_t len = strlen(str);
  char *dup = malloc(len + 1);
  strcpy(dup, str);
  return dup;
}

void calc_struct_size(StructInfo *sinfo);

int type_size(const Type *type) {
  switch (type->type) {
  case TY_VOID:
    return 1;  // ?
  case TY_CHAR:
    return 1;
  case TY_INT:
    return 4;
  case TY_PTR:
  case TY_FUNC:
    return 8;
  case TY_ARRAY:
    return type_size(type->ptrof) * type->array_size;
  case TY_STRUCT:
    if (type->struct_->size == 0)
      calc_struct_size(type->struct_);
    return type->struct_->size;
  default:
    assert(FALSE);
    return 1;
  }
}

int align_size(const Type *type) {
  switch (type->type) {
  case TY_VOID:
    return 1;  // ?
  case TY_CHAR:
    return 1;
  case TY_INT:
    return 4;
  case TY_PTR:
  case TY_FUNC:
    return 8;
  case TY_ARRAY:
    return align_size(type->ptrof);
  case TY_STRUCT:
    calc_struct_size(type->struct_);
    return type->struct_->align;
  default:
    assert(FALSE);
    return 1;
  }
}

void calc_struct_size(StructInfo *sinfo) {
  int size = 0;
  int max_align = 1;
  for (int i = 0, len = sinfo->members->len; i < len; ++i) {
    VarInfo *varinfo = (VarInfo*)sinfo->members->data[i];
    int sz = type_size(varinfo->type);
    int align = align_size(varinfo->type);
    size = (size + align - 1) & -align;
    varinfo->offset = (int)size;
    size += sz;
    if (max_align < align)
      max_align = align;
  }
  size = (size + max_align - 1) & -max_align;
  if (size == 0)
    size = 1;
  sinfo->size = size;
  sinfo->align = max_align;
}

void cast(const enum eType ltype, const enum eType rtype) {
  switch (ltype) {
  case TY_CHAR:
    switch (rtype) {
    case TY_CHAR:  return;
    case TY_INT:  return;
    default: break;
    }
    break;
  case TY_INT:
    switch (rtype) {
    case TY_INT:  return;
    case TY_CHAR: MOVSX_AL_RAX(); return;
    default: break;
    }
    break;
  case TY_PTR:
    switch (rtype) {
    case TY_PTR:  return;
    default: break;
    }
    break;
  default:
    break;
  }

  error("Cannot convert from type %d to %d", rtype, ltype);
}

Map *label_map;

enum LocType {
  LOC_REL32,
};

typedef struct {
  enum LocType type;
  uintptr_t ip;
  const char *label;
  union {
    struct {
      uintptr_t base;
    } rel;
  };
} LocInfo;

typedef struct {
  const char *label;
  const void *data;
  size_t size;
} RoData;

Vector *rodata_vector;

void add_rodata(const char *label, const void *data, size_t size) {
  RoData *ro = malloc(sizeof(*ro));
  ro->label = label;
  ro->data = data;
  ro->size = size;
  vec_push(rodata_vector, ro);
}

uintptr_t start_address;
unsigned char* code;
size_t codesize;

void add_code(const unsigned char* buf, size_t size) {
  size_t newsize = codesize + size;
  code = realloc(code, newsize);
  if (code == NULL)
    error("not enough memory");
  memcpy(code + codesize, buf, size);
  codesize = newsize;
}

// Put label at the current.
void add_label(const char *label) {
  map_put(label_map, (char*)label, (void*)CURIP(0));
}

char *alloc_label() {
  static int label_no;
  ++label_no;
  char buf[sizeof(int) * 3 + 1];
  snprintf(buf, sizeof(buf), ".L%d", label_no);
  char *dup = strdup_(buf);
  add_label(dup);
  return dup;
}

Vector *loc_vector;

LocInfo *new_loc(enum LocType type, uintptr_t ip, const char *label) {
  LocInfo *loc = malloc(sizeof(*loc));
  loc->type = type;
  loc->ip = ip;
  loc->label = label;
  vec_push(loc_vector, loc);
  return loc;
}

void add_loc_rel32(uintptr_t ip, const char *label, uintptr_t base) {
  LocInfo *loc = new_loc(LOC_REL32, ip, label);
  loc->rel.base = base;
}

size_t fixup_locations(void) {
  // Global
  for (int i = 0, len = map_count(global); i < len; ++i) {
    const char *name = (const char *)global->keys->data[i];
    const VarInfo *varinfo = (const VarInfo*)global->vals->data[i];
    if (varinfo->type->type == TY_FUNC)
      continue;
    add_label(name);
    int size = type_size(varinfo->type);
    unsigned char *buf = calloc(size, 1);
    add_code(buf, size);
  }

  for (int i = 0; i < loc_vector->len; ++i) {
    LocInfo *loc = loc_vector->data[i];
    void *val = map_get(label_map, loc->label);
    if (val == NULL) {
      error("Cannot find label: `%s'", loc->label);
    }

    intptr_t v = (intptr_t)val;
    switch (loc->type) {
    case LOC_REL32:
      {
        intptr_t d = v - loc->rel.base;
        // TODO: Check out of range
        code[loc->ip    ] = d;
        code[loc->ip + 1] = d >> 8;
        code[loc->ip + 2] = d >> 16;
        code[loc->ip + 3] = d >> 24;
      }
      break;
    default:
      assert(FALSE);
      break;
    }
  }

  return codesize;
}

static Node *curfunc;

void gen(Node *node);
void gen_lval(Node *node);

void gen_rval(Node *node) {
  gen(node);  // ?
}

void gen_ref(Node *node) {
  gen_lval(node);
}

void gen_lval(Node *node) {
  switch (node->type) {
  case ND_VARREF:
    if (node->varref.global) {
      LEA_OFS32_RIP_RAX(node->varref.ident);
    } else {
      int varidx = var_find(curfunc->defun.lvars, node->varref.ident);
      assert(varidx >= 0);
      int offset = ((VarInfo*)curfunc->defun.lvars->data[varidx])->offset;
      MOV_RBP_RAX();
      ADD_IM32_RAX(offset);
    }
    break;
  case ND_DEREF:
    gen_rval(node->unary.sub);
    break;
  case ND_MEMBER:
    {
      const Type *type = node->member.target->expType;
      if (type->type == TY_PTR)
        type = type->ptrof;
      assert(type->type == TY_STRUCT);
      Vector *members = type->struct_->members;
      int varidx = var_find(members, node->member.name);
      assert(varidx >= 0);
      VarInfo *varinfo = (VarInfo*)members->data[varidx];

      gen_ref(node->member.target);
      if (varinfo->offset != 0)
        ADD_IM32_RAX(varinfo->offset);
    }
    break;
  default:
    error("No lvalue: %d", node->type);
    break;
  }
}

void gen_cond_jmp(Node *cond, int tf, const char *label) {
  gen(cond);
  CMP_I8_EAX(0);
  if (tf)
    JNE32(label);
  else
    JE32(label);
}

void gen_defun(Node *node) {
  curfunc = node;
  add_label(node->defun.name);
  node->defun.ret_label = alloc_label();

  // Calc local variable offsets.
  // Map parameters from the bottom (to reduce offsets).
  int frame_size = 0;
  for (int i = 0; i < node->defun.lvars->len; ++i) {
    VarInfo *lvar = (VarInfo*)node->defun.lvars->data[i];
    int size = type_size(lvar->type);
    frame_size += size;
    lvar->offset = -frame_size;
  }

  // Prologue
  // Allocate variable bufer.
  Vector *lvars = node->defun.lvars;
  PUSH_RBP();
  MOV_RSP_RBP();
  if (frame_size > 0) {
    SUB_IM32_RSP(frame_size);
    // Store parameters into local frame.
    int len = len = node->defun.param_count;
    if (len > 6)
      error("Parameter count exceeds 6 (%d)", len);
    for (int i = 0; i < len; ++i) {
      int offset = ((VarInfo*)lvars->data[i])->offset;
      switch (i) {
      case 0:  MOV_EDI_IND8_RBP(offset); break;
      case 1:  MOV_ESI_IND8_RBP(offset); break;
      case 2:  MOV_EDX_IND8_RBP(offset); break;
      case 3:  MOV_ECX_IND8_RBP(offset); break;
      case 4:  MOV_R8D_IND8_RBP(offset); break;
      case 5:  MOV_R9D_IND8_RBP(offset); break;
      default: break;
      }
    }
  }

  // Statements
  for (int i = 0; i < node->defun.stmts->len; ++i) {
    gen((Node*)node->defun.stmts->data[i]);
  }

  // Epilogue
  add_label(node->defun.ret_label);
  MOV_RBP_RSP();
  POP_RBP();
  RET();
  curfunc = NULL;
}

void gen(Node *node) {
  switch (node->type) {
  case ND_NUM:
    MOV_I32_EAX(node->val);
    return;

  case ND_CHAR:
    MOV_I8_AL(node->val);
    return;

  case ND_STR:
    {
      const char * label = alloc_label();
      add_rodata(label, node->str, strlen(node->str) + 1);
      LEA_OFS32_RIP_RAX(label);
    }
    return;

  case ND_VARREF:
    {
      gen_lval(node);
      VarInfo *varinfo;
      if (node->varref.global) {
        varinfo = find_global(node->varref.ident);
      } else {
        int varidx = var_find(curfunc->defun.lvars, node->varref.ident);
        assert(varidx >= 0);
        varinfo = (VarInfo*)curfunc->defun.lvars->data[varidx];
      }
      if (varinfo->type->type != TY_ARRAY)  // If the variable is array, use variable address as a pointer.
        MOV_IND_RAX_RAX();
    }
    return;

  case ND_REF:
    gen_ref(node->unary.sub);
    return;

  case ND_DEREF:
    gen_rval(node->unary.sub);
    MOV_IND_RAX_RAX();
    return;

  case ND_MEMBER:
    gen_lval(node);
    switch (node->expType->type) {
    case TY_CHAR:
      MOV_IND_RAX_AL();
      break;
    case TY_INT:
      MOV_IND_RAX_EAX();
      break;
    case TY_PTR:
      MOV_IND_RAX_RAX();
      break;
    default:
      assert(FALSE);
      break;
    }
    return;

  case ND_ASSIGN:
    gen_lval(node->bop.lhs);
    PUSH_RAX();
    gen(node->bop.rhs);
    cast(node->bop.lhs->expType->type, node->bop.rhs->expType->type);

    POP_RDI();
    switch (node->bop.lhs->expType->type) {
    case TY_CHAR:
      MOV_AL_IND_RDI();
      break;
    case TY_INT:
      MOV_EAX_IND_RDI();
      break;
    case TY_PTR:
    default:
      MOV_RAX_IND_RDI();
      break;
    }
    return;

  case ND_DEFUN:
    gen_defun(node);
    return;

  case ND_RETURN:
    if (node->return_.val != NULL)
      gen(node->return_.val);
    assert(curfunc != NULL);
    JMP32(curfunc->defun.ret_label);
    return;

  case ND_FUNCALL:
    {
      Vector *args = node->funcall.args;
      if (args != NULL) {
        int len = args->len;
        if (len > 6)
          error("Param count exceeds 6 (%d)", len);

        for (int i = 0; i < len; ++i) {
          gen((Node*)args->data[i]);
          PUSH_RAX();
        }

        switch (len) {
        case 6:  POP_R9();  // Fall
        case 5:  POP_R8();  // Fall
        case 4:  POP_RCX();  // Fall
        case 3:  POP_RDX();  // Fall
        case 2:  POP_RSI();  // Fall
        case 1:  POP_RDI();  // Fall
        default: break;
        }
      }
      Node *func = node->funcall.func;
      if (func->type != ND_VARREF || !func->varref.global)
        error("Not implement");
      CALL(func->varref.ident);
      return;
    }

  case ND_BLOCK:
    for (int i = 0, len = node->block.nodes->len; i < len; ++i)
      gen((Node*)node->block.nodes->data[i]);
    break;

  case ND_IF:
    {
      const char * flabel = alloc_label();
      gen_cond_jmp(node->if_.cond, FALSE, flabel);
      gen(node->if_.tblock);
      if (node->if_.fblock != NULL) {
        const char * nlabel = alloc_label();
        JMP32(nlabel);
        add_label(flabel);
        gen(node->if_.fblock);
        add_label(nlabel);
      }
    }
    break;

  case ND_WHILE:
    {
      const char * llabel = alloc_label();
      const char * clabel = alloc_label();
      JMP32(clabel);
      add_label(llabel);
      gen(node->while_.body);
      add_label(clabel);
      gen_cond_jmp(node->while_.cond, TRUE, llabel);
    }
    break;

  case ND_DO_WHILE:
    {
      const char * llabel = alloc_label();
      add_label(llabel);
      gen(node->do_while.body);
      gen_cond_jmp(node->do_while.cond, TRUE, llabel);
    }
    break;

  case ND_FOR:
    {
      const char * l_cond = alloc_label();
      const char * l_break = alloc_label();
      if (node->for_.pre != NULL)
        gen(node->for_.pre);
      add_label(l_cond);
      if (node->for_.cond != NULL) {
        gen_cond_jmp(node->for_.cond, FALSE, l_break);
      }
      gen(node->for_.body);
      if (node->for_.post != NULL)
        gen(node->for_.post);
      JMP32(l_cond);
      add_label(l_break);
    }
    break;

  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_GT:
  case ND_LE:
  case ND_GE:
    {
      enum NodeType type = node->type;
      Node *lhs = node->bop.lhs;
      Node *rhs = node->bop.rhs;
      if (type == ND_LE || type == ND_GT) {
        Node *tmp = lhs; lhs = rhs; rhs = tmp;
        type = type == ND_LE ? ND_GE : ND_LT;
      }

      gen(lhs);
      PUSH_RAX();
      gen(rhs);

      POP_RDI();
      CMP_EAX_EDI();
      switch (type) {
      case ND_EQ:  SETE_AL(); break;
      case ND_NE:  SETNE_AL(); break;
      case ND_LT:  SETS_AL(); break;
      case ND_GE:  SETNS_AL(); break;
      default: assert(FALSE); break;
      }
    }
    MOVZX_AL_EAX();
    return;

  case ND_ADD:
    if (node->bop.lhs->expType->type == TY_PTR || node->bop.rhs->expType->type == TY_PTR) {
      Node *lhs = node->bop.lhs, *rhs = node->bop.rhs;
      if (rhs->expType->type == TY_PTR) {
        Node *tmp = lhs;
        lhs = rhs;
        rhs = tmp;
      }
      gen(rhs);
      long size = type_size(lhs->expType->ptrof);
      if (size != 1) {
        MOV_I32_EDI(size);
        MUL_EDI();
      }
      PUSH_RAX();
      gen(lhs);
      POP_RDI();
      ADD_RDI_RAX();
      break;
    }
    goto L_binop;

  case ND_SUB:
    if (node->bop.lhs->expType->type == TY_PTR) {
      int size = type_size(node->bop.lhs->expType->ptrof);
      if (node->bop.rhs->expType->type == TY_PTR) {
        gen(node->bop.rhs);
        PUSH_RAX();
        gen(node->bop.lhs);
        POP_RDI();
        SUB_RDI_RAX();

        switch (size) {
        case 1:  break;
        case 2:  SAR_RAX(); break;
        case 4:  SAR_IM8_RAX(2); break;
        case 8:  SAR_IM8_RAX(3); break;
        default:
          MOV_I64_RDI((long)size);
          MOV_I32_RDX(0);
          DIV_RDI();
          break;
        }
      } else {
        gen(node->bop.rhs);
        if (size != 1) {
          MOV_I64_RDI((long)size);
          MUL_RDI();
        }
        PUSH_RAX();
        gen(node->bop.lhs);
        POP_RDI();
        SUB_RDI_RAX();
      }
      break;
    }
    goto L_binop;

  case ND_MUL:
  case ND_DIV:
  case ND_MOD:
L_binop:
    gen(node->bop.rhs);
    PUSH_RAX();
    gen(node->bop.lhs);

    POP_RDI();

    switch (node->type) {
    case ND_ADD:
      ADD_EDI_EAX();
      break;
    case ND_SUB:
      SUB_EDI_EAX();
      break;
    case ND_MUL:
      MUL_EDI();
      break;
    case ND_DIV:
      MOV_I32_RDX(0);
      DIV_EDI();
      break;
    case ND_MOD:
      MOV_I32_RDX(0);
      DIV_EDI();
      MOV_EDX_EAX();
      break;
    default:
      assert(FALSE);
      break;
    }
    return;

  default:
    error("Unhandled node: %d", node->type);
    break;
  }
}

void compile(const char* source) {
  tokenize(source);
  program();

  for (int i = 0, len = node_vector->len; i < len; ++i)
    gen(node_vector->data[i]);

  // Output RoData
  for (int i = 0, len = rodata_vector->len; i < len; ++i) {
    const RoData *ro = (const RoData*)rodata_vector->data[i];
    add_label(ro->label);
    add_code(ro->data, ro->size);
  }
}

void output_code(FILE* fp) {
  fwrite(code, codesize, 1, fp);
}
