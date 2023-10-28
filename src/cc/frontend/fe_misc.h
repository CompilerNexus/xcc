#pragma once

#include <stdbool.h>

#include "ast.h"  // ExprKind
#include "type.h"  // FixnumKind

typedef struct Expr Expr;
typedef struct MemberInfo MemberInfo;
typedef struct Name Name;
typedef struct Scope Scope;
typedef struct Token Token;
typedef struct Type Type;
typedef struct VarInfo VarInfo;
typedef struct Vector Vector;

extern Function *curfunc;
extern Scope *curscope;

extern bool error_warning;
extern int compile_warning_count;
extern int compile_error_count;

enum ParseErrorLevel {
  PE_WARNING,
  PE_NOFATAL,
  PE_FATAL,
};

typedef struct {
  Stmt *swtch;
  Stmt *break_;
  Stmt *continu;
} LoopScope;

#define SAVE_LOOP_SCOPE(var, b, c)  LoopScope var = loop_scope; if (b != NULL) loop_scope.break_ = b; if (c != NULL) loop_scope.continu = c;
#define RESTORE_LOOP_SCOPE(var)     loop_scope = var

extern LoopScope loop_scope;

void parse_error(enum ParseErrorLevel level, const Token *token, const char *fmt, ...);

void not_void(const Type *type, const Token *token);
void not_const(const Type *type, const Token *token);

typedef struct {
  int storage, qualifier;
  int unsigned_num, signed_num;
  int char_num, short_num, int_num, long_num;
  int float_num, double_num;
} TypeCombination;

extern const enum FixnumKind kLongKinds[];

void check_type_combination(const TypeCombination *tc, const Token *tok);
bool no_type_combination(const TypeCombination *tc, int storage_mask, int qualifier_mask);

VarInfo *find_var_from_scope(Scope *scope, const Token *ident, Type *type, int storage);
VarInfo *add_var_to_scope(Scope *scope, const Token *ident, Type *type, int storage);
Expr *alloc_tmp_var(Scope *scope, Type *type);
void define_enum_member(Type *type, const Token *ident, int value);

Scope *enter_scope(Function *func, Vector *vars);
void exit_scope(void);

void ensure_struct(Type *type, const Token *token, Scope *scope);
bool check_cast(const Type *dst, const Type *src, bool zero, bool is_explicit, const Token *token);
Expr *make_cast(Type *type, const Token *token, Expr *sub, bool is_explicit);
const MemberInfo *search_from_anonymous(const Type *type, const Name *name, const Token *ident,
                                        Vector *stack);
void check_lval(const Token *tok, Expr *expr, const char *error);
Expr *reduce_refer(Expr *expr);
Expr *make_refer(const Token *tok, Expr *expr);
Expr *promote_to_int(Expr *expr);
Expr *new_expr_num_bop(enum ExprKind kind, const Token *tok, Expr *lhs, Expr *rhs);
Expr *new_expr_int_bop(enum ExprKind kind, const Token *tok, Expr *lhs, Expr *rhs);
Expr *new_expr_addsub(enum ExprKind kind, const Token *tok, Expr *lhs, Expr *rhs);
#ifndef __NO_BITFIELD
void not_bitfield_member(Expr *expr);
Expr *extract_bitfield_value(Expr *src, const MemberInfo *minfo);
Expr *assign_bitfield_member(const Token *tok, Expr *dst, Expr *src, Expr *val,
                             const MemberInfo *minfo);
Expr *assign_to_bitfield(const Token *tok, Expr *lhs, Expr *rhs, const MemberInfo *minfo);
#else
#define not_bitfield_member(expr)  // Must not, so noop.
#endif
Expr *incdec_of(enum ExprKind kind, Expr *target, const Token *tok);
Expr *new_expr_cmp(enum ExprKind kind, const Token *tok, Expr *lhs, Expr *rhs);
Expr *make_cond(Expr *expr);
Expr *make_not_expr(Expr *expr);
void check_funcall_args(Expr *func, Vector *args, Scope *scope);
Vector *extract_varinfo_types(const Vector *params);  // <VarInfo*> => <Type*>
Type *choose_ternary_result_type(Expr *tval, Expr *fval);
Expr *transform_assign_with(const Token *tok, Expr *lhs, Expr *rhs);

void check_func_reachability(Function *func);
bool check_funcend_return(Stmt *stmt);

int get_funparam_index(Function *func, const Name *name);  // -1: Not funparam.

bool satisfy_inline_criteria(const VarInfo *varinfo);
Stmt *embed_inline_funcall(VarInfo *varinfo);
