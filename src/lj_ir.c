/*
** SSA IR (Intermediate Representation) emitter.
** Copyright (C) 2005-2009 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_ir_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_HASJIT

#include "lj_gc.h"
#include "lj_str.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_iropt.h"
#include "lj_trace.h"

/* Some local macros to save typing. Undef'd at the end. */
#define IR(ref)			(&J->cur.ir[(ref)])
#define fins			(&J->fold.ins)

/* Pass IR on to next optimization in chain (FOLD). */
#define emitir(ot, a, b)        (lj_ir_set(J, (ot), (a), (b)), lj_opt_fold(J))

/* -- IR tables ----------------------------------------------------------- */

/* IR instruction modes. */
LJ_DATADEF const uint8_t lj_ir_mode[IR__MAX+1] = {
IRDEF(IRMODE)
  0
};

/* -- IR emitter ---------------------------------------------------------- */

/* Grow IR buffer at the top. */
void LJ_FASTCALL lj_ir_growtop(jit_State *J)
{
  IRIns *baseir = J->irbuf + J->irbotlim;
  MSize szins = J->irtoplim - J->irbotlim;
  if (szins) {
    baseir = (IRIns *)lj_mem_realloc(J->L, baseir, szins*sizeof(IRIns),
				     2*szins*sizeof(IRIns));
    J->irtoplim = J->irbotlim + 2*szins;
  } else {
    baseir = (IRIns *)lj_mem_realloc(J->L, NULL, 0, LJ_MIN_IRSZ*sizeof(IRIns));
    J->irbotlim = REF_BASE - LJ_MIN_IRSZ/4;
    J->irtoplim = J->irbotlim + LJ_MIN_IRSZ;
  }
  J->cur.ir = J->irbuf = baseir - J->irbotlim;
}

/* Grow IR buffer at the bottom or shift it up. */
static void lj_ir_growbot(jit_State *J)
{
  IRIns *baseir = J->irbuf + J->irbotlim;
  MSize szins = J->irtoplim - J->irbotlim;
  lua_assert(szins != 0);
  lua_assert(J->cur.nk == J->irbotlim);
  if (J->cur.nins + (szins >> 1) < J->irtoplim) {
    /* More than half of the buffer is free on top: shift up by a quarter. */
    MSize ofs = szins >> 2;
    memmove(baseir + ofs, baseir, (J->cur.nins - J->irbotlim)*sizeof(IRIns));
    J->irbotlim -= ofs;
    J->irtoplim -= ofs;
    J->cur.ir = J->irbuf = baseir - J->irbotlim;
  } else {
    /* Double the buffer size, but split the growth amongst top/bottom. */
    IRIns *newbase = lj_mem_newt(J->L, 2*szins*sizeof(IRIns), IRIns);
    MSize ofs = szins >= 256 ? 128 : (szins >> 1);  /* Limit bottom growth. */
    memcpy(newbase + ofs, baseir, (J->cur.nins - J->irbotlim)*sizeof(IRIns));
    lj_mem_free(G(J->L), baseir, szins*sizeof(IRIns));
    J->irbotlim -= ofs;
    J->irtoplim = J->irbotlim + 2*szins;
    J->cur.ir = J->irbuf = newbase - J->irbotlim;
  }
}

/* Emit IR without any optimizations. */
TRef LJ_FASTCALL lj_ir_emit(jit_State *J)
{
  IRRef ref = lj_ir_nextins(J);
  IRIns *ir = IR(ref);
  IROp op = fins->o;
  ir->prev = J->chain[op];
  J->chain[op] = (IRRef1)ref;
  ir->o = op;
  ir->op1 = fins->op1;
  ir->op2 = fins->op2;
  J->guardemit.irt |= fins->t.irt;
  return TREF(ref, irt_t((ir->t = fins->t)));
}

/* -- Interning of constants ---------------------------------------------- */

/*
** IR instructions for constants are kept between J->cur.nk >= ref < REF_BIAS.
** They are chained like all other instructions, but grow downwards.
** The are interned (like strings in the VM) to facilitate reference
** comparisons. The same constant must get the same reference.
*/

/* Get ref of next IR constant and optionally grow IR.
** Note: this may invalidate all IRIns *!
*/
static LJ_AINLINE IRRef ir_nextk(jit_State *J)
{
  IRRef ref = J->cur.nk;
  if (LJ_UNLIKELY(ref <= J->irbotlim)) lj_ir_growbot(J);
  J->cur.nk = --ref;
  return ref;
}

/* Intern int32_t constant. */
TRef LJ_FASTCALL lj_ir_kint(jit_State *J, int32_t k)
{
  IRIns *ir, *cir = J->cur.ir;
  IRRef ref;
  for (ref = J->chain[IR_KINT]; ref; ref = cir[ref].prev)
    if (cir[ref].i == k)
      goto found;
  ref = ir_nextk(J);
  ir = IR(ref);
  ir->i = k;
  ir->t.irt = IRT_INT;
  ir->o = IR_KINT;
  ir->prev = J->chain[IR_KINT];
  J->chain[IR_KINT] = (IRRef1)ref;
found:
  return TREF(ref, IRT_INT);
}

/* The MRef inside the KNUM IR instruction holds the address of the constant
** (an aligned double or a special 64 bit pattern). The KNUM constants
** themselves are stored in a chained array and shared across traces.
**
** Rationale for choosing this data structure:
** - The address of the constants is embedded in the generated machine code
**   and must never move. A resizable array or hash table wouldn't work.
** - Most apps need very few non-integer constants (less than a dozen).
** - Linear search is hard to beat in terms of speed and low complexity.
*/
typedef struct KNumArray {
  MRef next;			/* Pointer to next list. */
  MSize numk;			/* Number of used elements in this array. */
  TValue k[LJ_MIN_KNUMSZ];	/* Array of constants. */
} KNumArray;

/* Free all chained arrays. */
void lj_ir_knum_freeall(jit_State *J)
{
  KNumArray *kn;
  for (kn = mref(J->knum, KNumArray); kn; ) {
    KNumArray *next = mref(kn->next, KNumArray);
    lj_mem_free(J2G(J), kn, sizeof(KNumArray));
    kn = next;
  }
}

/* Find KNUM constant in chained array or add it. */
static cTValue *ir_knum_find(jit_State *J, uint64_t nn)
{
  KNumArray *kn, *knp = NULL;
  TValue *ntv;
  MSize idx;
  /* Search for the constant in the whole chain of arrays. */
  for (kn = mref(J->knum, KNumArray); kn; kn = mref(kn->next, KNumArray)) {
    knp = kn;  /* Remember previous element in list. */
    for (idx = 0; idx < kn->numk; idx++) {  /* Search one array. */
      TValue *tv = &kn->k[idx];
      if (tv->u64 == nn)  /* Needed for +-0/NaN/absmask. */
	return tv;
    }
  }
  /* Constant was not found, need to add it. */
  if (!(knp && knp->numk < LJ_MIN_KNUMSZ)) {  /* Allocate a new array. */
    KNumArray *nkn = lj_mem_newt(J->L, sizeof(KNumArray), KNumArray);
    setmref(nkn->next, NULL);
    nkn->numk = 0;
    if (knp)
      setmref(knp->next, nkn);  /* Chain to the end of the list. */
    else
      setmref(J->knum, nkn);  /* Link first array. */
    knp = nkn;
  }
  ntv = &knp->k[knp->numk++];  /* Add to current array. */
  ntv->u64 = nn;
  return ntv;
}

/* Intern FP constant, given by its address. */
TRef lj_ir_knum_addr(jit_State *J, cTValue *tv)
{
  IRIns *ir, *cir = J->cur.ir;
  IRRef ref;
  for (ref = J->chain[IR_KNUM]; ref; ref = cir[ref].prev)
    if (ir_knum(&cir[ref]) == tv)
      goto found;
  ref = ir_nextk(J);
  ir = IR(ref);
  setmref(ir->ptr, tv);
  ir->t.irt = IRT_NUM;
  ir->o = IR_KNUM;
  ir->prev = J->chain[IR_KNUM];
  J->chain[IR_KNUM] = (IRRef1)ref;
found:
  return TREF(ref, IRT_NUM);
}

/* Intern FP constant, given by its 64 bit pattern. */
TRef lj_ir_knum_nn(jit_State *J, uint64_t nn)
{
  return lj_ir_knum_addr(J, ir_knum_find(J, nn));
}

/* Special 16 byte aligned SIMD constants. */
LJ_DATADEF LJ_ALIGN(16) cTValue lj_ir_knum_tv[4] = {
  { U64x(7fffffff,ffffffff) }, { U64x(7fffffff,ffffffff) },
  { U64x(80000000,00000000) }, { U64x(80000000,00000000) }
};

/* Check whether a number is int and return it. -0 is NOT considered an int. */
static int numistrueint(lua_Number n, int32_t *kp)
{
  int32_t k = lj_num2int(n);
  if (n == cast_num(k)) {
    if (kp) *kp = k;
    if (k == 0) {  /* Special check for -0. */
      TValue tv;
      setnumV(&tv, n);
      if (tv.u32.hi != 0)
	return 0;
    }
    return 1;
  }
  return 0;
}

/* Intern number as int32_t constant if possible, otherwise as FP constant. */
TRef lj_ir_knumint(jit_State *J, lua_Number n)
{
  int32_t k;
  if (numistrueint(n, &k))
    return lj_ir_kint(J, k);
  else
    return lj_ir_knum(J, n);
}

/* Intern GC object "constant". */
TRef lj_ir_kgc(jit_State *J, GCobj *o, IRType t)
{
  IRIns *ir, *cir = J->cur.ir;
  IRRef ref;
  lua_assert(!isdead(J2G(J), o));
  for (ref = J->chain[IR_KGC]; ref; ref = cir[ref].prev)
    if (ir_kgc(&cir[ref]) == o)
      goto found;
  ref = ir_nextk(J);
  ir = IR(ref);
  /* NOBARRIER: Current trace is a GC root. */
  setgcref(ir->gcr, o);
  ir->t.irt = (uint8_t)t;
  ir->o = IR_KGC;
  ir->prev = J->chain[IR_KGC];
  J->chain[IR_KGC] = (IRRef1)ref;
found:
  return TREF(ref, t);
}

/* Intern 32 bit pointer constant. */
TRef lj_ir_kptr(jit_State *J, void *ptr)
{
  IRIns *ir, *cir = J->cur.ir;
  IRRef ref;
  lua_assert((void *)(intptr_t)i32ptr(ptr) == ptr);
  for (ref = J->chain[IR_KPTR]; ref; ref = cir[ref].prev)
    if (mref(cir[ref].ptr, void) == ptr)
      goto found;
  ref = ir_nextk(J);
  ir = IR(ref);
  setmref(ir->ptr, ptr);
  ir->t.irt = IRT_PTR;
  ir->o = IR_KPTR;
  ir->prev = J->chain[IR_KPTR];
  J->chain[IR_KPTR] = (IRRef1)ref;
found:
  return TREF(ref, IRT_PTR);
}

/* Intern typed NULL constant. */
TRef lj_ir_knull(jit_State *J, IRType t)
{
  IRIns *ir, *cir = J->cur.ir;
  IRRef ref;
  for (ref = J->chain[IR_KNULL]; ref; ref = cir[ref].prev)
    if (irt_t(cir[ref].t) == t)
      goto found;
  ref = ir_nextk(J);
  ir = IR(ref);
  ir->i = 0;
  ir->t.irt = (uint8_t)t;
  ir->o = IR_KNULL;
  ir->prev = J->chain[IR_KNULL];
  J->chain[IR_KNULL] = (IRRef1)ref;
found:
  return TREF(ref, t);
}

/* Intern key slot. */
TRef lj_ir_kslot(jit_State *J, TRef key, IRRef slot)
{
  IRIns *ir, *cir = J->cur.ir;
  IRRef2 op12 = IRREF2((IRRef1)key, (IRRef1)slot);
  IRRef ref;
  /* Const part is not touched by CSE/DCE, so 0-65535 is ok for IRMlit here. */
  lua_assert(tref_isk(key) && slot == (IRRef)(IRRef1)slot);
  for (ref = J->chain[IR_KSLOT]; ref; ref = cir[ref].prev)
    if (cir[ref].op12 == op12)
      goto found;
  ref = ir_nextk(J);
  ir = IR(ref);
  ir->op12 = op12;
  ir->t.irt = IRT_PTR;
  ir->o = IR_KSLOT;
  ir->prev = J->chain[IR_KSLOT];
  J->chain[IR_KSLOT] = (IRRef1)ref;
found:
  return TREF(ref, IRT_PTR);
}

/* -- Access to IR constants ---------------------------------------------- */

/* Copy value of IR constant. */
void lj_ir_kvalue(lua_State *L, TValue *tv, const IRIns *ir)
{
  UNUSED(L);
  lua_assert(ir->o != IR_KSLOT);  /* Common mistake. */
  if (irt_isint(ir->t)) {
    lua_assert(ir->o == IR_KINT);
    setintV(tv, ir->i);
  } else if (irt_isnum(ir->t)) {
    lua_assert(ir->o == IR_KNUM);
    setnumV(tv, ir_knum(ir)->n);
  } else if (irt_ispri(ir->t)) {
    lua_assert(ir->o == IR_KPRI);
    setitype(tv, irt_toitype(ir->t));
  } else {
    if (ir->o == IR_KGC) {
      lua_assert(irt_isgcv(ir->t));
      setgcV(L, tv, &ir_kgc(ir)->gch, irt_toitype(ir->t));
    } else {
      lua_assert(ir->o == IR_KPTR || ir->o == IR_KNULL);
      setlightudV(tv, mref(ir->ptr, void));
    }
  }
}

/* -- Convert IR operand types -------------------------------------------- */

/* Convert from integer or string to number. */
TRef LJ_FASTCALL lj_ir_tonum(jit_State *J, TRef tr)
{
  if (!tref_isnum(tr)) {
    if (tref_isinteger(tr))
      tr = emitir(IRTN(IR_TONUM), tr, 0);
    else if (tref_isstr(tr))
      tr = emitir(IRTG(IR_STRTO, IRT_NUM), tr, 0);
    else
      lj_trace_err(J, LJ_TRERR_BADTYPE);
  }
  return tr;
}

/* Convert from integer or number to string. */
TRef LJ_FASTCALL lj_ir_tostr(jit_State *J, TRef tr)
{
  if (!tref_isstr(tr)) {
    if (!tref_isnumber(tr))
      lj_trace_err(J, LJ_TRERR_BADTYPE);
    tr = emitir(IRT(IR_TOSTR, IRT_STR), tr, 0);
  }
  return tr;
}

/* Convert from number or string to bitop operand (overflow wrapped). */
TRef LJ_FASTCALL lj_ir_tobit(jit_State *J, TRef tr)
{
  if (!tref_isinteger(tr)) {
    if (tref_isstr(tr))
      tr = emitir(IRTG(IR_STRTO, IRT_NUM), tr, 0);
    else if (!tref_isnum(tr))
      lj_trace_err(J, LJ_TRERR_BADTYPE);
    tr = emitir(IRTI(IR_TOBIT), tr, lj_ir_knum_tobit(J));
  }
  return tr;
}

/* Convert from number or string to integer (overflow undefined). */
TRef LJ_FASTCALL lj_ir_toint(jit_State *J, TRef tr)
{
  if (!tref_isinteger(tr)) {
    if (tref_isstr(tr))
      tr = emitir(IRTG(IR_STRTO, IRT_NUM), tr, 0);
    else if (!tref_isnum(tr))
      lj_trace_err(J, LJ_TRERR_BADTYPE);
    tr = emitir(IRTI(IR_TOINT), tr, IRTOINT_ANY);
  }
  return tr;
}

/* -- Miscellaneous IR ops ------------------------------------------------ */

/* Evaluate numeric comparison. */
int lj_ir_numcmp(lua_Number a, lua_Number b, IROp op)
{
  switch (op) {
  case IR_EQ: return (a == b);
  case IR_NE: return (a != b);
  case IR_LT: return (a < b);
  case IR_GE: return (a >= b);
  case IR_LE: return (a <= b);
  case IR_GT: return (a > b);
  case IR_ULT: return !(a >= b);
  case IR_UGE: return !(a < b);
  case IR_ULE: return !(a > b);
  case IR_UGT: return !(a <= b);
  default: lua_assert(0); return 0;
  }
}

/* Evaluate string comparison. */
int lj_ir_strcmp(GCstr *a, GCstr *b, IROp op)
{
  int res = lj_str_cmp(a, b);
  switch (op) {
  case IR_LT: return (res < 0);
  case IR_GE: return (res >= 0);
  case IR_LE: return (res <= 0);
  case IR_GT: return (res > 0);
  default: lua_assert(0); return 0;
  }
}

/* Rollback IR to previous state. */
void lj_ir_rollback(jit_State *J, IRRef ref)
{
  IRRef nins = J->cur.nins;
  while (nins > ref) {
    IRIns *ir;
    nins--;
    ir = IR(nins);
    J->chain[ir->o] = ir->prev;
  }
  J->cur.nins = nins;
}

#undef IR
#undef fins
#undef emitir

#endif
