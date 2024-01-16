#include "../config.h"
#include "wcc.h"

#include <assert.h>
#include <stdlib.h>  // malloc
#include <string.h>  // memcpy

#include "ast.h"
#include "fe_misc.h"  // curscope
#include "lexer.h"
#include "table.h"
#include "type.h"
#include "util.h"
#include "var.h"

const char BREAK_ADDRESS_NAME[] = "__curbrk";
const char VA_ARGS_NAME[] = ".._VA_ARGS";

Table builtin_function_table;
Table unresolved_gvar_table;

static Stmt *branching_stmt;

bool is_stack_param(const Type *type) {
  return !is_prim_type(type);
}

static void wasm_func_type(const Type *type, DataStorage *ds) {
  bool ret_param = type->func.ret->kind != TY_VOID && !is_prim_type(type->func.ret);
  const Vector *params = type->func.params;
  int param_count = 0;
  if (params != NULL) {
    for (int i = 0; i < params->len; ++i) {
      const Type *type = params->data[i];
      if (!is_stack_param(type))
        ++param_count;
    }
  }

  data_init(ds);
  data_reserve(ds, 3 + param_count + 3);

  data_uleb128(ds, -1, (int)ret_param + param_count + (type->func.vaargs ? 1 : 0));  // num params
  if (ret_param)
    data_push(ds, to_wtype(&tyVoidPtr));
  if (params != NULL) {
    for (int i = 0; i < params->len; ++i) {
      const Type *type = params->data[i];
      if (!is_stack_param(type))
        data_push(ds, to_wtype(type));
    }
  }
  if (type->func.vaargs)
    data_push(ds, to_wtype(&tyVoidPtr));  // vaarg pointer.

  if (type->func.ret->kind == TY_VOID) {
    data_push(ds, 0);  // num results
  } else if (ret_param) {
    data_push(ds, 1);  // num results
    data_push(ds, to_wtype(&tyVoidPtr));
  } else {
    data_push(ds, 1);  // num results
    data_push(ds, to_wtype(type->func.ret));
  }
}

int getsert_func_type_index(const Type *type, bool reg) {
  DataStorage ds;
  wasm_func_type(type, &ds);
  return getsert_func_type(ds.buf, ds.len, reg);
}

extern int get_func_type_index(const Type *type);

static FuncInfo *register_func_info(const Name *funcname, Function *func, int flag) {
  assert(func == NULL || func->type->kind == TY_FUNC);
  FuncInfo *info;
  if (!table_try_get(&func_info_table, funcname, (void**)&info)) {
    info = calloc_or_die(sizeof(*info));
    table_put(&func_info_table, funcname, info);
    info->type_index = (uint32_t)-1;

    VarInfo *varinfo = scope_find(global_scope, funcname, NULL);
    assert(varinfo != NULL);
    assert(varinfo->type->kind == TY_FUNC);
    assert(func == NULL || same_type(varinfo->type, func->type));
    info->varinfo = varinfo;
  }
  if (func != NULL)
    info->func = func;
  if (info->type_index == (uint32_t)-1)
    info->type_index = getsert_func_type_index(info->varinfo->type, true);
  info->flag |= flag;
  return info;
}

// static void register_func_info_if_not_exist(const Name *funcname, Type *(*callback)(void)) {
//   if (table_get(&func_info_table, funcname) == NULL) {
//     Type *functype = (*callback)();
//     scope_add(global_scope, funcname, functype, 0);
//     register_func_info(funcname, NULL, FF_REFERRED);
//   }
// }

static uint32_t register_indirect_function(const Name *name) {
  FuncInfo *info;
  if (table_try_get(&indirect_function_table, name, (void**)&info))
    return info->indirect_index;

  info = register_func_info(name, NULL, FF_INDIRECT | FF_REFERRED);
  uint32_t index = indirect_function_table.count + 1;
  info->indirect_index = index;
  table_put(&indirect_function_table, name, info);
  return index;
}

GVarInfo *get_gvar_info(Expr *expr) {
  assert(expr->kind == EX_VAR);
  Scope *scope;
  VarInfo *varinfo = scope_find(expr->var.scope, expr->var.name, &scope);
  assert(varinfo != NULL && scope == expr->var.scope);
  if (!is_global_scope(scope)) {
    if (varinfo->storage & VS_EXTERN) {
      varinfo = scope_find(scope = global_scope, expr->var.name, &scope);
    } else if (varinfo->storage & VS_STATIC) {
      varinfo = varinfo->static_.gvar;
    }
  }
  GVarInfo *info = get_gvar_info_from_name(varinfo->name);
  if (info == NULL) {
    table_put(&unresolved_gvar_table, varinfo->name, varinfo);
    // Returns dummy.
    info = register_gvar_info(varinfo->name, varinfo);
    info->flag |= GVF_UNRESOLVED;
  }
  return info;
}

#define add_global_var(type, name)  scope_add(global_scope, name, type, 0)

void add_builtin_function(const char *str, Type *type, BuiltinFunctionProc *proc,
                          bool add_to_scope) {
  const Name *name = alloc_name(str, NULL, false);
  table_put(&builtin_function_table, name, proc);

  if (add_to_scope)
    scope_add(global_scope, name, type, 0);
}

static void traverse_stmts(Vector *stmts);
static void traverse_stmt(Stmt *stmt);

// l=r  =>  (t=r, l=t)
static Expr *assign_to_tmp(Expr *assign, Expr **ptmp) {
  assert(assign->kind == EX_ASSIGN);
  const Token *token = assign->token;
  Type *type = assign->bop.lhs->type;
  Expr *rhs = assign->bop.rhs;
  Expr *tmp = alloc_tmp_var(curscope, type);
  *ptmp = tmp;
  Expr *assign_tmp = new_expr_bop(EX_ASSIGN, &tyVoid, token, tmp, rhs);
  assign->bop.rhs = tmp;
  return new_expr_bop(EX_COMMA, type, token, assign_tmp, assign);
}

static void traverse_expr(Expr **pexpr, bool needval);

static void traverse_func_expr(Expr **pexpr) {
  Expr *expr = *pexpr;
  const Type *type = expr->type;
  assert(type->kind == TY_FUNC ||
         (type->kind == TY_PTR && type->pa.ptrof->kind == TY_FUNC));
  if (expr->kind == EX_VAR) {
    bool global = false;
    if (is_global_scope(expr->var.scope)) {
      global = true;
    } else {
      Scope *scope;
      VarInfo *varinfo = scope_find(expr->var.scope, expr->var.name, &scope);
      global = (varinfo->storage & VS_EXTERN) != 0;
    }
    if (global && type->kind == TY_FUNC) {
      BuiltinFunctionProc *proc;
      if (!table_try_get(&builtin_function_table, expr->var.name, (void**)&proc))
        register_func_info(expr->var.name, NULL, FF_REFERRED);
      else
        (*proc)(expr, BFP_TRAVERSE);
    } else {
      assert(type->kind == TY_PTR && type->pa.ptrof->kind == TY_FUNC);
      getsert_func_type_index(type->pa.ptrof, true);
    }
  } else {
    traverse_expr(pexpr, true);
  }
}

static void traverse_funcall(Expr **pexpr, bool needval) {
  Expr *expr = *pexpr;
  UNUSED(needval);

  Expr *func = expr->funcall.func;
  Type *functype = get_callee_type(func->type);
  if (functype == NULL) {
    parse_error(PE_NOFATAL, func->token, "Cannot call except function");
    return;
  }
  if (functype->func.ret->kind != TY_VOID && !is_prim_type(functype->func.ret)) {
    // Allocate local variable for return value.
    assert(curfunc != NULL);
    assert(curfunc->scopes != NULL);
    assert(curfunc->scopes->len > 0);
    VarInfo *varinfo = add_var_to_scope(curfunc->scopes->data[0], alloc_dummy_ident(),
                                        functype->func.ret, 0);
    FuncExtra *extra = curfunc->extra;
    assert(extra != NULL);
    if (extra->funcall_results == NULL)
      extra->funcall_results = new_vector();
    vec_push(extra->funcall_results, expr);
    vec_push(extra->funcall_results, varinfo);
  }

  Vector *args = expr->funcall.args;
  if (functype->func.params == NULL) {
    if (func->kind == EX_VAR) {
      // Extract function type again.
      VarInfo *varinfo = scope_find(func->var.scope, func->var.name, NULL);
      assert(varinfo != NULL);
      if (varinfo->type->kind == TY_FUNC) {
        func->type = functype = varinfo->type;
        if (functype->func.params != NULL)  // Updated.
          check_funcall_args(func, args, curscope);
      }
    }

    if (functype->func.params == NULL)
      parse_error(PE_NOFATAL, func->token, "function's parameters must be known");
  }

  traverse_func_expr(&expr->funcall.func);
  for (int i = 0, n = args->len; i < n; ++i)
    traverse_expr((Expr**)&args->data[i], true);
}

static void te_noop(Expr **pexpr, bool needval) { UNUSED(pexpr); UNUSED(needval); }

static void te_var(Expr **pexpr, bool needval) {
  Expr *expr = *pexpr;
  UNUSED(needval);
  if (expr->type->kind == TY_FUNC) {
    register_indirect_function(expr->var.name);
    traverse_func_expr(pexpr);
  } else {
    if (out_type < OutExecutable && is_global_scope(expr->var.scope)) {
      // Register used global variable even if the entity is `extern`.
      get_gvar_info(expr);
    }
  }
}

static void te_bop(Expr **pexpr, bool needval) {
  Expr *expr = *pexpr;
  traverse_expr(&expr->bop.lhs, needval);
  traverse_expr(&expr->bop.rhs, needval);
}

static void te_shift(Expr **pexpr, bool needval) {
  Expr *expr = *pexpr;
  // Make sure that RHS type is same as LHS.
  expr->bop.rhs = make_cast(expr->bop.lhs->type, expr->bop.rhs->token, expr->bop.rhs, false);
  traverse_expr(&expr->bop.lhs, needval);
  traverse_expr(&expr->bop.rhs, needval);
}

static void te_comma(Expr **pexpr, bool needval) {
  Expr *expr = *pexpr;
  traverse_expr(&expr->bop.lhs, false);
  traverse_expr(&expr->bop.rhs, needval);
}

static void te_assign(Expr **pexpr, bool needval) {
  Expr *expr = *pexpr;
  traverse_expr(&expr->bop.lhs, false);
  traverse_expr(&expr->bop.rhs, true);
  expr->type = &tyVoid;  // Make assigment expression as void.
  if (needval) {
    Expr *rhs = expr->bop.rhs;
    Type *type = rhs->type;
    if (!(is_const(rhs) || rhs->kind == EX_VAR)) {  // Rhs may have side effect.
      Expr *lhs = expr->bop.lhs;
      if (lhs->kind == EX_VAR) {
        type = lhs->type;
        rhs = lhs;
      } else {
        Expr *tmp;
        expr = assign_to_tmp(expr, &tmp);
        rhs = tmp;
      }
    }
    *pexpr = new_expr_bop(EX_COMMA, type, expr->token, expr, rhs);
  }
}

static void te_unary(Expr **pexpr, bool needval) {
  Expr *expr = *pexpr;
  traverse_expr(&expr->unary.sub, needval);
}

static void te_cast(Expr **pexpr, bool needval) {
  Expr *expr = *pexpr;
  traverse_expr(&expr->unary.sub, needval);

  Expr *src = expr->unary.sub;
  Type *stype = src->type;
  Type *dtype = expr->type;
  bool du = dtype->kind != TY_FIXNUM || dtype->fixnum.is_unsigned;
  bool su = stype->kind != TY_FIXNUM || stype->fixnum.is_unsigned;
  if (su && !du) {  // signed <- unsigned.
    int d = type_size(dtype);
    int s = type_size(stype);
    if (d == s && d < I32_SIZE) {
      // This conversion requires conditional branch with MSB.
      // Stack machine architecture cannot handle the case well, so replace the expression.
      Expr *assign = NULL;
      if (src->kind != EX_VAR) {
        // To use the src value multiple times, store it to temporary variable.
        Expr *orgsrc = src;
        const Name *name = alloc_label();
        scope_add(curscope, name, stype, 0);
        Expr *var = new_expr_variable(name, stype, NULL, curscope);
        assign = new_expr_bop(EX_ASSIGN, &tyVoid, NULL, var, orgsrc);
        src = var;
      }
      // sign: src & (1 << (s * TARGET_CHAR_BIT - 1))
      Expr *msb = new_expr_bop(EX_BITAND, stype, NULL, src,
                               new_expr_fixlit(stype, NULL, 1 << (s * TARGET_CHAR_BIT - 1)));
      // src | -msb
      Expr *replaced = new_expr_bop(EX_BITOR, dtype, NULL, src,
                                    new_expr_unary(EX_NEG, stype, NULL, msb));
      *pexpr = assign == NULL ? replaced : new_expr_bop(EX_COMMA, dtype, NULL, assign, replaced);
      // traverse_expr(pexpr, needval);
    }
  }
}

static void te_incdec(Expr **pexpr, bool needval) {
  Expr *expr = *pexpr;

  static const enum ExprKind kOpAddSub[2] = {EX_ADD, EX_SUB};
  traverse_expr(&expr->unary.sub, needval);
  Expr *target = expr->unary.sub;
  if (target->kind == EX_COMPLIT)
    target = target->complit.var;
  if (target->kind == EX_VAR) {
    VarInfo *varinfo = scope_find(target->var.scope, target->var.name, NULL);
    if (!(varinfo->storage & VS_REF_TAKEN) && !is_global_datsec_var(varinfo, target->var.scope))
      return;
  }

  Type *type = target->type;
  assert(is_number(type) || type->kind == TY_PTR);
  bool post = expr->kind >= EX_POSTINC;
  bool dec = (expr->kind - EX_PREINC) & 1;
  // (++xxx)  =>  (p = &xxx, tmp = *p + 1, *p = tmp, tmp)
  // (xxx++)  =>  (p = &xxx, tmp = *p, *p = tmp + 1, tmp)
  const Token *token = target->token;
  Type *ptrtype = ptrof(type);
  Expr *p = alloc_tmp_var(curscope, ptrtype);
  Expr *tmp = alloc_tmp_var(curscope, type);
  enum ExprKind op = kOpAddSub[dec];

  Expr *assign_p = new_expr_bop(EX_ASSIGN, &tyVoid, token, p, make_refer(token, target));
  Expr *deref_p = new_expr_deref(token, p);
  Expr *one = type->kind == TY_PTR ? new_expr_fixlit(&tySize, token, type_size(type->pa.ptrof))
#ifndef __NO_FLONUM
                                   : type->kind == TY_FLONUM ? new_expr_flolit(type, token, 1)
#endif
                                                             : new_expr_fixlit(type, token, 1);
  Expr *assign_tmp = new_expr_bop(EX_ASSIGN, &tyVoid, token, tmp,
                                  !post ? new_expr_bop(op, type, token, deref_p, one) : deref_p);
  Expr *assign_deref_p = new_expr_bop(EX_ASSIGN, &tyVoid, token, deref_p,
                                      !post ? tmp : new_expr_bop(op, type, token, tmp, one));

  *pexpr = new_expr_bop(EX_COMMA, type, token, assign_p,
                        new_expr_bop(EX_COMMA, type, token, assign_tmp,
                                     new_expr_bop(EX_COMMA, type, token, assign_deref_p, tmp)));
}

static void te_ternary(Expr **pexpr, bool needval) {
  Expr *expr = *pexpr;
  traverse_expr(&expr->ternary.cond, true);
  traverse_expr(&expr->ternary.tval, needval);
  traverse_expr(&expr->ternary.fval, needval);
}

static void te_member(Expr **pexpr, bool needval) {
  Expr *expr = *pexpr;
  traverse_expr(&expr->member.target, needval);
}

static void te_complit(Expr **pexpr, bool needval) {
  Expr *expr = *pexpr;
  UNUSED(needval);
  if (expr->complit.inits != NULL)
    traverse_stmts(expr->complit.inits);
}

static void te_block(Expr **pexpr, bool needval) {
  Expr *expr = *pexpr;
  UNUSED(needval);
  traverse_stmt(expr->block);
}

static void te_inlined(Expr **pexpr, bool needval) {
  Expr *expr = *pexpr;
  UNUSED(needval);
  Vector *args = expr->inlined.args;
  for (int i = 0; i < args->len; ++i)
    traverse_expr((Expr**)&args->data[i], true);
  traverse_stmt(expr->inlined.embedded);
}

static void traverse_expr(Expr **pexpr, bool needval) {
  Expr *expr = *pexpr;
  if (expr == NULL)
    return;

  typedef void (*TraverseExprFunc)(Expr **, bool);
  static const TraverseExprFunc table[] = {
    [EX_FIXNUM] = te_noop, [EX_FLONUM] = te_noop, [EX_STR] = te_noop, [EX_VAR] = te_var,
    [EX_ADD] = te_bop, [EX_SUB] = te_bop, [EX_MUL] = te_bop, [EX_DIV] = te_bop, [EX_MOD] = te_bop,
    [EX_BITAND] = te_bop, [EX_BITOR] = te_bop, [EX_BITXOR] = te_bop,
    [EX_EQ] = te_bop, [EX_NE] = te_bop, [EX_LT] = te_bop,
    [EX_LE] = te_bop, [EX_GE] = te_bop, [EX_GT] = te_bop,
    [EX_LOGAND] = te_bop, [EX_LOGIOR] = te_bop, [EX_LSHIFT] = te_shift, [EX_RSHIFT] = te_shift,
    [EX_POS] = te_unary, [EX_NEG] = te_unary, [EX_BITNOT] = te_unary,
    [EX_REF] = te_unary, [EX_DEREF] = te_unary,
    [EX_ASSIGN] = te_assign, [EX_COMMA] = te_comma,
    [EX_PREINC] = te_incdec, [EX_PREDEC] = te_incdec, [EX_POSTINC] = te_incdec, [EX_POSTDEC] = te_incdec,
    [EX_CAST] = te_cast, [EX_TERNARY] = te_ternary, [EX_MEMBER] = te_member,
    [EX_FUNCALL] = traverse_funcall, [EX_INLINED] = te_inlined, [EX_COMPLIT] = te_complit,
    [EX_BLOCK] = te_block,
  };

  assert(expr->kind < (int)sizeof(table) / sizeof(*table));
  assert(table[expr->kind] != NULL);
  (*table[expr->kind])(pexpr, needval);
}

static void traverse_initializer(Initializer *init) {
  if (init == NULL)
    return;

  switch (init->kind) {
  case IK_SINGLE:
    traverse_expr(&init->single, true);
    break;
  case IK_MULTI:
    {
      Vector *multi = init->multi;
      for (int i = 0; i < multi->len; ++i)
        traverse_initializer(multi->data[i]);
    }
    break;
  case IK_DOT:
    traverse_initializer(init->dot.value);
    break;
  case IK_BRKT:
    traverse_initializer(init->bracket.value);
    break;
  }
}

static Stmt *push_branching_stmt(Stmt *stmt) {
  Stmt *prev = branching_stmt;
  branching_stmt = stmt;
  return prev;
}

static void traverse_if(Stmt *stmt) {
  traverse_expr(&stmt->if_.cond, true);
  Stmt *saved = push_branching_stmt(stmt);
  traverse_stmt(stmt->if_.tblock);
  traverse_stmt(stmt->if_.fblock);
  branching_stmt = saved;
}

static void traverse_switch(Stmt *stmt) {
  traverse_expr(&stmt->switch_.value, true);
  Stmt *saved = push_branching_stmt(stmt);
  traverse_stmt(stmt->switch_.body);
  branching_stmt = saved;

  if (!is_const(stmt->switch_.value) && stmt->switch_.value->kind != EX_VAR) {
    Expr *org_value = stmt->switch_.value;
    // Store value into temporary variable.
    assert(curfunc != NULL);
    Scope *scope = curfunc->scopes->data[0];
    const Name *name = alloc_label();
    Type *type = stmt->switch_.value->type;
    scope_add(scope, name, type, 0);

    // switch (complex)  =>  switch ((tmp = complex, tmp))
    Expr *var = new_expr_variable(name, type, NULL, scope);
    Expr *comma = new_expr_bop(
        EX_COMMA, type, org_value->token,
        new_expr_bop(EX_ASSIGN, &tyVoid, org_value->token, var, org_value),
        var);
    stmt->switch_.value = comma;
  }
}

static void traverse_case(Stmt *stmt) {
  if (branching_stmt->kind != ST_SWITCH)
    parse_error(PE_FATAL, stmt->token, "case/default inside branch not supported");
}

static void traverse_while(Stmt *stmt) {
  traverse_expr(&stmt->while_.cond, true);
  Stmt *saved = push_branching_stmt(stmt);
  traverse_stmt(stmt->while_.body);
  branching_stmt = saved;
}

static void traverse_do_while(Stmt *stmt) {
  Stmt *saved = push_branching_stmt(stmt);
  traverse_stmt(stmt->while_.body);
  branching_stmt = saved;
  traverse_expr(&stmt->while_.cond, true);
}

static void traverse_for(Stmt *stmt) {
  traverse_expr(&stmt->for_.pre, false);
  traverse_expr(&stmt->for_.cond, true);
  traverse_expr(&stmt->for_.post, false);
  Stmt *saved = push_branching_stmt(stmt);
  traverse_stmt(stmt->for_.body);
  branching_stmt = saved;
}

static void traverse_vardecl(Stmt *stmt) {
  Vector *decls = stmt->vardecl.decls;
  if (decls != NULL) {
    for (int i = 0, n = decls->len; i < n; ++i) {
      VarDecl *decl = decls->data[i];
      if (decl->ident != NULL) {
        VarInfo *varinfo = scope_find(curscope, decl->ident, NULL);
        assert(varinfo != NULL);
        if (!(varinfo->storage & (VS_EXTERN | VS_STATIC)))
          traverse_initializer(varinfo->local.init);
      }
      traverse_stmt(decl->init_stmt);
    }
  }
}

static void traverse_stmt(Stmt *stmt) {
  if (stmt == NULL)
    return;
  switch (stmt->kind) {
  case ST_EMPTY: break;
  case ST_EXPR:  traverse_expr(&stmt->expr, false); break;
  case ST_RETURN:
    traverse_expr(&stmt->return_.val, true);
    break;
  case ST_BLOCK:
    {
      Scope *bak = NULL;
      if (stmt->block.scope != NULL)
        curscope = bak = stmt->block.scope;
      traverse_stmts(stmt->block.stmts);
      if (bak != NULL)
        curscope = bak;
    }
    break;
  case ST_IF:  traverse_if(stmt); break;
  case ST_SWITCH:  traverse_switch(stmt); break;
  case ST_CASE: traverse_case(stmt); break;
  case ST_WHILE:  traverse_while(stmt); break;
  case ST_DO_WHILE:  traverse_do_while(stmt); break;
  case ST_FOR:  traverse_for(stmt); break;
  case ST_BREAK:  break;
  case ST_CONTINUE:  break;
  case ST_GOTO:
    parse_error(PE_FATAL, stmt->token, "cannot use goto");
    break;
  case ST_LABEL:  traverse_stmt(stmt->label.stmt); break;
  case ST_VARDECL:  traverse_vardecl(stmt); break;
  case ST_ASM:  break;
  }
}

static void traverse_stmts(Vector *stmts) {
  assert(stmts != NULL);
  for (int i = 0, len = stmts->len; i < len; ++i) {
    Stmt *stmt = stmts->data[i];
    traverse_stmt(stmt);
  }
}

static void traverse_defun(Function *func) {
  if (func->scopes == NULL)  // Prototype definition
    return;

  FuncExtra *extra = calloc_or_die(sizeof(FuncExtra));
  extra->reloc_code = new_vector();
  func->extra = extra;

  Type *functype = func->type;
  assert(func->params != NULL);
  if (equal_name(func->name, alloc_name("main", NULL, false))) {
    // Force `main' function takes two arguments.
    if (func->params->len < 1) {
      assert(func->scopes->len > 0);
      const Name *name = alloc_label();
      Type *type = &tyInt;
      Scope *scope = func->scopes->data[0];
      vec_push((Vector*)func->params, scope_add(scope, name, type, 0));
      vec_push((Vector*)functype->func.params, type);
    }
    if (func->params->len < 2) {
      assert(func->scopes->len > 0);
      Type *type = ptrof(ptrof(&tyChar));
      const Name *name = alloc_label();
      Scope *scope = func->scopes->data[0];
      vec_push((Vector*)func->params, scope_add(scope, name, type, 0));
      vec_push((Vector*)functype->func.params, type);
    }
  }
  if (functype->func.vaargs) {
    Type *tyvalist = find_typedef(curscope, alloc_name("__builtin_va_list", NULL, false), NULL);
    assert(tyvalist != NULL);

    const Name *name = alloc_name(VA_ARGS_NAME, NULL, false);
    scope_add(func->scopes->data[0], name, tyvalist, 0);
  }

  register_func_info(func->name, func, 0);
  curfunc = func;
  traverse_stmt(func->body_block);
  if (compile_error_count == 0) {
    int count = ((FuncExtra*)func->extra)->setjmp_count;
    if (count > 0)
      modify_ast_for_setjmp(count);
  }
  curfunc = NULL;

  // Static variables are traversed through global variables.
}

static void traverse_decl(Declaration *decl) {
  if (decl == NULL)
    return;

  switch (decl->kind) {
  case DCL_DEFUN:
    traverse_defun(decl->defun.func);
    break;
  case DCL_VARDECL:
    break;
  case DCL_ASM:
    parse_error(PE_NOFATAL, decl->asmstr->token, "`__asm` not allowed");
    break;
  }
}

static void add_builtins(void) {
  // Stack pointer.
  {
    const Name *name = alloc_name(SP_NAME, NULL, false);
    VarInfo *varinfo = add_global_var(&tyVoidPtr, name);
    Initializer *init = new_initializer(IK_SINGLE, NULL);
    init->single = new_expr_fixlit(varinfo->type, NULL, 0);  // Dummy
    varinfo->global.init = init;

    GVarInfo *info = register_gvar_info(varinfo->name, varinfo);
    if (out_type < OutExecutable)
      info->flag |= GVF_UNRESOLVED;
  }

  // Break address.
  if (out_type >= OutExecutable) {
    const Name *name = alloc_name(BREAK_ADDRESS_NAME, NULL, false);
    VarInfo *varinfo = add_global_var(&tyVoidPtr, name);
    Initializer *init = new_initializer(IK_SINGLE, NULL);
    init->single = new_expr_fixlit(varinfo->type, NULL, 0);  // Dummy
    varinfo->global.init = init;

    register_gvar_info(varinfo->name, varinfo);
  }
}

uint32_t traverse_ast(Vector *decls, Vector *exports, uint32_t stack_size) {
  // Global scope
  for (int i = 0, len = global_scope->vars->len; i < len; ++i) {
    VarInfo *varinfo = global_scope->vars->data[i];
    if (varinfo->storage & (VS_EXTERN | VS_ENUM_MEMBER) || varinfo->type->kind == TY_FUNC)
      continue;
    register_gvar_info(varinfo->name, varinfo);
    traverse_initializer(varinfo->global.init);
  }

  add_builtins();

  for (int i = 0, len = decls->len; i < len; ++i) {
    Declaration *decl = decls->data[i];
    traverse_decl(decl);
  }

  // Check exports
  for (int i = 0; i < exports->len; ++i) {
    const Name *name = exports->data[i];
    FuncInfo *info = table_get(&func_info_table, name);
    if (info == NULL)
      error("`%.*s' not found", NAMES(name));
    register_func_info(name, NULL, FF_REFERRED);
  }

  // Linking information.
  if (out_type < OutExecutable) {
    uint32_t symbol_index = 0;
    const Name *name;
    FuncInfo *info;
    for (int it = 0; (it = table_iterate(&func_info_table, it, &name, (void**)&info)) != -1; ) {
      if (info->flag == 0)
        continue;
      ++symbol_index;
    }

    // Assign linking index to globals.
    uint32_t global_index = 0;
    uint32_t data_index = 0;
    for (int i = 0, len = global_scope->vars->len; i < len; ++i) {
      VarInfo *varinfo = global_scope->vars->data[i];
      if (varinfo->storage & VS_ENUM_MEMBER || varinfo->type->kind == TY_FUNC)
        continue;
      GVarInfo *info = get_gvar_info_from_name(varinfo->name);
      if (info == NULL)
        continue;
      uint32_t index = !is_global_datsec_var(varinfo, global_scope) ? global_index++ : !(varinfo->storage & VS_EXTERN) ? data_index++ : (uint32_t)-1;
      info->non_prim.item_index = index;
      info->non_prim.symbol_index = symbol_index++;
    }
  }

  {
    // Enumerate functions.
    VERBOSES("### Functions\n");
    const Name *name;
    FuncInfo *info;
    int32_t index = 0;
    for (int k = 0; k < 2; ++k) {  // 0: import, 1: defined-and-referred
      for (int it = 0; (it = table_iterate(&func_info_table, it, &name, (void**)&info)) != -1; ) {
        if ((k == 0 && info->func != NULL) ||
            (k != 0 && (info->func == NULL || info->flag == 0)))
          continue;
        info->index = index++;
        VERBOSE("%2d: %.*s%s\n", info->index, NAMES(name), k == 0 ? "  (import)" : "");
      }
    }
    VERBOSES("\n");
  }

  uint32_t sp_bottom;
  {
    // Enumerate global variables.
    const uint32_t START_ADDRESS = 1;  // Avoid valid poiter is NULL.
    uint32_t address = START_ADDRESS;
    const Name *name;
    GVarInfo *info;

    VERBOSE("### Memory  0x%x\n", address);
    for (int k = 0; k < 2; ++k) {  // 0: data, 1: bss
      if (k == 1)
        VERBOSE("---- BSS  0x%x\n", address);
      for (int it = 0; (it = table_iterate(&gvar_info_table, it, &name, (void**)&info)) != -1; ) {
        const VarInfo *varinfo = info->varinfo;
        if ((varinfo->global.init == NULL) == (k == 0) || !is_global_datsec_var(varinfo, global_scope))
          continue;
        // Mapped to memory
        address = ALIGN(address, align_size(varinfo->type));
        info->non_prim.address = address;
        size_t size = type_size(varinfo->type);
        address += size;
        VERBOSE("%04x: %.*s  (size=0x%lx)\n", info->non_prim.address, NAMES(name), size);
      }
    }

    // Primitive types (Globals).
    VERBOSES("\n### Globals\n");
    uint32_t index = 0;
    for (int it = 0; (it = table_iterate(&gvar_info_table, it, &name, (void**)&info)) != -1; ) {
      const VarInfo *varinfo = info->varinfo;
      if (is_global_datsec_var(varinfo, global_scope))
        continue;
      info->prim.index = index++;
      VERBOSE("%2d: %.*s\n", info->prim.index, NAMES(name));
    }
    VERBOSES("\n");

    // Set initial values.
    sp_bottom = ALIGN(address + stack_size, 16);
    if (out_type >= OutExecutable) {
      {  // Stack pointer.
        VarInfo *varinfo = scope_find(global_scope, alloc_name(SP_NAME, NULL, false), NULL);
        assert(varinfo != NULL);
        Initializer *init = varinfo->global.init;
        assert(init != NULL && init->kind == IK_SINGLE && init->single->kind == EX_FIXNUM);
        init->single->fixnum = sp_bottom;
        VERBOSE("SP bottom: 0x%x  (size=0x%x)\n", sp_bottom, stack_size);
      }
      {  // Break address.
        VarInfo *varinfo = scope_find(global_scope, alloc_name(BREAK_ADDRESS_NAME, NULL, false), NULL);
        assert(varinfo != NULL);
        Initializer *init = varinfo->global.init;
        assert(init != NULL && init->kind == IK_SINGLE && init->single->kind == EX_FIXNUM);
        init->single->fixnum = sp_bottom;
        VERBOSE("Break address: 0x%x\n", sp_bottom);
      }
    }
  }

  return sp_bottom;
}
