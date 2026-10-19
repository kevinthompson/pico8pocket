// Guarded inheritance stays outside the whole-program optimizer and hot text.
#include <stdint.h>
#include "lua.h"
#include "ldo.h"
#include "lgc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lvm.h"
#if defined(__ELF__)
#define P8P_VM_COLD __attribute__((noinline, section(".p8p_vm_cold")))
#else
#define P8P_VM_COLD __attribute__((noinline))
#endif
static_assert(sizeof(Value) == sizeof(uintptr_t), "callback payload ABI");

/** @brief Read an interned short-string key without allocating or mutating.
 * Mirrors ltable.c bucket/chain lookup without retaining the public luaH_getstr
 * ABI. Returns a borrowed table slot, or luaO_nilobject when the key is absent.
 */
static inline __attribute__((always_inline)) const TValue *delegate_raw(Table *t, TString *key) {
  Node *n = gnode(t, lmod(key->tsv.hash, sizenode(t)));
  do {
    if (ttisshrstring(gkey(n)) && eqshrstr(rawtsvalue(gkey(n)), key))
      return gval(n);
    n = gnext(n);
  } while (n);
  return luaO_nilobject;
}

/** @brief Resolve a read through at most four exact, pure Lua delegates.
 * Accepts only warmed function(self, key) return self.<constant field>[key] end
 * closures with sufficient existing call/stack capacity, no observable hooks
 * and no pending GC work. Count-only hooks are safe: this VM decrements their
 * counter only at servicepoints (calls, jumps and loops), and the exact bytecode
 * pattern below contains none. Nested accepted delegates also contain none.
 * See z8lua-servicepoints.patch and z8lua-pc-register.patch for hook dispatch.
 * Does not allocate, collect or invoke cart code.
 * Returns a borrowed non-nil slot (including false), or NULL to require the
 * ordinary metamethod call. Copy the slot into a rooted value before allocation,
 * cart calls or table mutation. A miss may cache the absence of __index in
 * metatable flags; it does not alter the Lua stack or callback state.
 */
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
    base = delegate_raw(hvalue(t), rawtsvalue(&p->k[constant]));
    if (!ttistable(base)) return NULL;
    value = delegate_raw(hvalue(base), rawtsvalue(key));
    if (!ttisnil(value)) return value;  /* false is a successful hit */
    t = base;
    Table *metatable = hvalue(t)->metatable;
    if (metatable == NULL || (metatable->flags & (1u << TM_INDEX)))
      return NULL;
    tm = delegate_raw(metatable, G(L)->tmname[TM_INDEX]);
    if (ttisnil(tm)) {
      metatable->flags |= cast_byte(1u << TM_INDEX);
      return NULL;
    }
  }
  return NULL;
}


/** @brief Exercise the scalar callback ABI through the retained cold thunk.
 * Used by the contract tests; production callTM uses luaV_delegateclosure.
 * Retained with the measured image layout. Reconstructs the complete Value/tag
 * pair, tries the guard for result-bearing calls, then roots the callback and
 * arguments before a normal Lua call. With hasres == 1, p3 receives the result
 * after restoring its stack offset; with hasres == 0, p3 is the third argument.
 * Yield/error behavior follows luaD_call and the current Lua/C caller context.
 */
P8P_VM_COLD
void luaV_callindex(lua_State *L, Value function_value, int function_tag,
                    const TValue *p1, const TValue *p2, TValue *p3, int hasres) {
  TValue function;
  function.value_ = function_value;
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

/** @brief Adapt a complete scalar Value/tag pair for guard contract tests.
 * This retained adapter preserves the measured image layout; production uses
 * the typed closure entry. Borrows operands and returns the delegate guard's
 * borrowed slot or NULL fallback without pushing a callback onto the stack.
 */
P8P_VM_COLD
const TValue *luaV_delegatebits(lua_State *L, Value function_value,
                                int function_tag, const TValue *p1,
                                const TValue *p2) {
  TValue function;
  function.value_ = function_value;
  function.tt_ = function_tag;
  return luaV_delegateget(L, p1, p2, &function);
}

/** @brief Enter the delegate guard after callTM proves a Lua closure tag.
 * The typed pointer avoids aggregate ABI coupling and inactive union reads.
 * Borrows the closure and operands; returns a borrowed non-nil slot on success
 * or NULL when callTM must execute its ordinary callback path.
 */
P8P_VM_COLD
const TValue *luaV_delegateclosure(lua_State *L, LClosure *closure,
                                   const TValue *p1, const TValue *p2) {
  TValue function;
  setclLvalue(L, &function, closure);
  return luaV_delegateget(L, p1, p2, &function);
}
