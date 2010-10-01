/*
** DCE: Dead Code Elimination. Pre-LOOP only -- ASM already performs DCE.
** Copyright (C) 2005-2009 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_opt_dce_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_HASJIT

#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_iropt.h"

/* Some local macros to save typing. Undef'd at the end. */
#define IR(ref)		(&J->cur.ir[(ref)])

/* Scan through all snapshots and mark all referenced instructions. */
static void dce_marksnap(jit_State *J)
{
  SnapNo i, nsnap = J->cur.nsnap;
  for (i = 0; i < nsnap; i++) {
    SnapShot *snap = &J->cur.snap[i];
    IRRef2 *map = &J->cur.snapmap[snap->mapofs];
    BCReg s, nslots = snap->nslots;
    for (s = 0; s < nslots; s++) {
      IRRef ref = snap_ref(map[s]);
      if (!irref_isk(ref))
	irt_setmark(IR(ref)->t);
    }
  }
}

/* Backwards propagate marks. Replace unused instructions with NOPs. */
static void dce_propagate(jit_State *J)
{
  IRRef1 *pchain[IR__MAX];
  IRRef ins;
  uint32_t i;
  for (i = 0; i < IR__MAX; i++) pchain[i] = &J->chain[i];
  for (ins = J->cur.nins-1; ins >= REF_FIRST; ins--) {
    IRIns *ir = IR(ins);
    if (irt_ismarked(ir->t)) {
      irt_clearmark(ir->t);
      pchain[ir->o] = &ir->prev;
    } else if (!(irt_isguard(ir->t) || irm_sideeff(lj_ir_mode[ir->o]))) {
      *pchain[ir->o] = ir->prev;  /* Reroute original instruction chain. */
      *pchain[IR_NOP] = (IRRef1)ins;
      ir->t.irt = IRT_NIL;
      ir->o = IR_NOP;  /* Replace instruction with NOP. */
      ir->op1 = ir->op2 = 0;
      pchain[IR_NOP] = &ir->prev;
      continue;
    }
    if (!irref_isk(ir->op1)) irt_setmark(IR(ir->op1)->t);
    if (!irref_isk(ir->op2)) irt_setmark(IR(ir->op2)->t);
  }
  *pchain[IR_NOP] = 0;  /* Terminate NOP chain. */
}

/* Dead Code Elimination.
**
** First backpropagate marks for all used instructions. Then replace
** the unused ones with a NOP. Note that compressing the IR to eliminate
** the NOPs does not pay off.
*/
void lj_opt_dce(jit_State *J)
{
  if ((J->flags & JIT_F_OPT_DCE)) {
    dce_marksnap(J);
    dce_propagate(J);
  }
}

#undef IR

#endif
