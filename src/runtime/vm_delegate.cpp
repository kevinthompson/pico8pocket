// Guarded inheritance stays outside the whole-program optimizer and hot text.
#include <stdint.h>
#include <string.h>
#include "lua.h"
#include "ldo.h"
#include "lgc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "ltable.h"
#include "ltm.h"
#include "lvm.h"
#if defined(__ELF__)
#define P8P_VM_COLD __attribute__((noinline, section(".p8p_vm_cold")))
#else
#define P8P_VM_COLD __attribute__((noinline))
#endif
static_assert(sizeof(Value) == sizeof(uintptr_t), "callback payload ABI");

/* Resolve only successful, allocation-free reads through exact pure Lua
** delegates: function(self, key) return self.<constant field>[key] end.
** Failed speculation leaves the ordinary metamethod call untouched. */
P8P_VM_COLD
const TValue *luaV_delegateget (lua_State *L, const TValue *t,
                              const TValue *key, const TValue *tm) {
  int depth;
  CallInfo *ci = L->ci;
  if (!ttisshrstring(key) || (L->hookmask & ~LUA_MASKCOUNT) ||
      G(L)->GCdebt > 0)
    return NULL;
  for (depth = 0; depth < 4; ++depth) {
    Proto *p;
    Instruction first;
    int constant;
    const TValue *base, *value;
    ci = ci->next;
    if (ci == NULL) return NULL;  /* luaE_extendCI could allocate or fail */
    if (!ttistable(t) || !ttisLclosure(tm) ||
        L->nCcalls + depth + 1 >= LUAI_MAXCCALLS ||
        L->stack_last - L->top <= 4 * (depth + 1) + 3)
      return NULL;
    p = clLvalue(tm)->p;
    /* Keep first-call cache allocation, stack growth, GC checks and all
       observable hook/error paths in the normal interpreter. */
    if (!p->icache || p->numparams != 2 || p->is_vararg ||
        p->sizeupvalues != 0 || p->maxstacksize != 3 || p->sizecode != 4)
      return NULL;
    first = p->code[0];
    if (GET_OPCODE(first) != OP_GETTABLE || GETARG_A(first) != 2 ||
        GETARG_B(first) != 0 || !ISK(GETARG_C(first)) ||
        p->code[1] != CREATE_ABC(OP_GETTABLE, 2, 2, 1) ||
        p->code[2] != CREATE_ABC(OP_RETURN, 2, 2, 0) ||
        p->code[3] != CREATE_ABC(OP_RETURN, 0, 1, 0))
      return NULL;
    constant = INDEXK(GETARG_C(first));
    if (constant >= p->sizek || !ttisshrstring(&p->k[constant]))
      return NULL;
    base = luaH_getstr(hvalue(t), rawtsvalue(&p->k[constant]));
    if (!ttistable(base)) return NULL;
    value = luaH_getstr(hvalue(base), rawtsvalue(key));
    if (!ttisnil(value)) return value;  /* false is a successful hit */
    t = base;
    tm = fasttm(L, hvalue(t)->metatable, TM_INDEX);
    if (tm == NULL) return NULL;
  }
  return NULL;
}


// Match the original RV32 callTM IPA clone's seven-word calling convention.
// Only this cold boundary reconstructs the function TValue; the lookup loop
// retains scalar payload/tag arguments and its original register pressure.
P8P_VM_COLD
void luaV_callindex(lua_State *L, uintptr_t function_bits, int function_tag,
                    const TValue *p1, const TValue *p2, TValue *p3, int hasres) {
  TValue function;
  __builtin_memcpy(&function.value_, &function_bits, sizeof(function_bits));
  function.tt_ = function_tag;
  if (hasres) {
    const TValue *value = luaV_delegateget(L, p1, p2, &function);
    if (value != NULL) {
      setobj2s(L, p3, value);
      return;
    }
  }
  // Root the reconstructed callback and both arguments before any Lua call.
  ptrdiff_t result = savestack(L, p3);
  setobj2s(L, L->top++, &function);
  setobj2s(L, L->top++, p1);
  setobj2s(L, L->top++, p2);
  if (!hasres) {
    setobj2s(L, L->top++, p3);
  }
  luaD_call(L, L->top - (4 - hasres), hasres, isLua(L->ci));
  if (hasres) {
    p3 = restorestack(L, result);
    setobjs2s(L, p3, --L->top);
  }
}
