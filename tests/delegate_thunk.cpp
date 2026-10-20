#include <stdint.h>
#include <cstdio>
#include <cstring>
#include <cassert>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lstate.h"
#include "lobject.h"
#include "lvm.h"
/** @brief Return 37 to identify the light-C callback payload in ABI tests. */
static int light(lua_State*L){lua_pushinteger(L,37);return 1;}
/** @brief Return the captured upvalue to verify C-closure payload dispatch. */
static int closure(lua_State*L){lua_pushvalue(L,lua_upvalueindex(1));return 1;}
/** @brief Run fixture code with no results, asserting on load/runtime errors.
 * Borrows the Lua state and preserves stack values below the loaded chunk.
 */
static void eval(lua_State*L,const char*s){int rc=luaL_loadstring(L,s)||lua_pcall(L,0,0,0);if(rc){fprintf(stderr,"%s\n",lua_tostring(L,-1));assert(false);}}
/** @brief Call the retained thunk using the top four Lua stack slots.
 * Requires callback, table, key and value/result slots in that order. hasres
 * selects whether the fourth slot receives a result or supplies argument three.
 */
static void invoke(lua_State*L,int hasres){
 static_assert(sizeof(Value)==sizeof(uintptr_t),"function payload ABI");
 TValue*f=L->top-4,*t=L->top-3,*k=L->top-2,*v=L->top-1;luaV_callindex(L,f->value_,f->tt_,t,k,v,hasres);
}
/** @brief Validate VM callback arguments and count each invocation.
 * Assignment receives a third argument of 9 and returns nothing. Other calls
 * return 17 for a light-C callback or the captured value for a C closure.
 */
static int vm_callback(lua_State *L) {
 assert(lua_gettop(L)==2 || lua_gettop(L)==3);
 assert(lua_istable(L,1));
 lua_getglobal(L,"callback_count");
 int calls=lua_tointeger(L,-1);lua_pop(L,1);
 lua_pushinteger(L,calls+1);lua_setglobal(L,"callback_count");
 if(lua_gettop(L)==3){
  assert(lua_tointeger(L,3)==9);
  return 0;
 }
 lua_pushinteger(L,lua_isnone(L,lua_upvalueindex(1))?17:lua_tointeger(L,lua_upvalueindex(1)));
 return 1;
}
/** @brief Verify the five real VM callback paths using a borrowed Lua state.
 * Checks light-C and captured C callbacks for index, assignment, addition,
 * equality and length; also verifies callable-table dispatch and invalid
 * callback errors. Fixture globals are replaced in the supplied state.
 */
static void vm_callback_contracts(lua_State *L) {
 for(int captured=0;captured<2;++captured){
  if(captured){lua_pushinteger(L,41);lua_pushcclosure(L,vm_callback,1);}
  else lua_pushcfunction(L,vm_callback);
  lua_setglobal(L,"callback");
  eval(L,captured?"expected_callback=41":"expected_callback=17");
  eval(L,"callback_count=0 local mt={__index=callback,__newindex=callback,__add=callback,__eq=callback,__len=callback} local a,b=setmetatable({},mt),setmetatable({},mt) assert(a.missing==expected_callback) a.assigned=9 assert(rawget(a,'assigned')==nil) assert(a+b==expected_callback) assert(a==b) assert(#a==expected_callback) assert(callback_count==5)");
 }
 eval(L,"local calls=0 local callback=setmetatable({},{__call=function(self,a,b) calls=calls+1 return 59 end}) local mt={__add=callback,__eq=callback,__len=callback} local a,b=setmetatable({},mt),setmetatable({},mt) assert(a+b==59) assert(a==b) assert(#a==59) assert(calls==3)");
 eval(L,"local a=setmetatable({},{__add=7}) local ok,msg=pcall(function()return a+a end) assert(not ok and type(msg)=='string') invalid_callback_error=msg");
 lua_getglobal(L,"invalid_callback_error");
 assert(strstr(lua_tostring(L,-1),"number"));lua_pop(L,1);
 puts("PASS five VM callback paths with light-C and captured C callbacks");
}
static int count_hook_calls;

/** @brief Mutate the delegated table when a real count-hook event occurs. */
static void mutate_on_count(lua_State *L, lua_Debug *) {
 ++count_hook_calls;
 lua_getglobal(L,"B");
 lua_pushinteger(L,99);
 lua_setfield(L,-2,"x");
 lua_pop(L,1);
}

/** @brief Compare count-hook effects of guarded reads and ordinary Lua calls.
 * This VM counts servicepoints, not instructions. Exact GETTABLE/GETTABLE/
 * RETURN delegates have none, even with a hook due at the next servicepoint.
 * A loop-bearing delegate proves the hook can mutate the result on fallback.
 */
static void count_hook_contracts(lua_State *L) {
 for(int depth=1;depth<=4;depth+=3){
  eval(L,"D=function(self,k)return self.base[k]end B={x=7} O=setmetatable({base=B},{__index=D}) assert(O.x==7)");
  if(depth==4)eval(L,"for i=1,3 do O=setmetatable({base=O},{__index=D}) end assert(O.x==7)");
  for(int period=1;period<=2;++period){
   for(int ordinary=0;ordinary<2;++ordinary){
    eval(L,"B.x=7");
    if(ordinary)lua_getglobal(L,"D");
    lua_getglobal(L,"O");
    if(ordinary)lua_pushliteral(L,"x");
    count_hook_calls=0;
    lua_sethook(L,mutate_on_count,LUA_MASKCOUNT,period);
    G(L)->GCdebt=-1;
    // Calling D from C enters its ordinary Lua body without an outer OP_CALL.
    // The guarded case enters __index through the production lookup boundary.
    if(ordinary)lua_call(L,2,1);
    else lua_getfield(L,-1,"x");
    assert(lua_tointeger(L,-1)==7);
    assert(count_hook_calls==0 && L->hookcount==period);
    lua_sethook(L,NULL,0,0);
    lua_settop(L,0);
   }
  }
 }
 eval(L,"D=function(self,k)for i=1,1 do end return self.base[k]end B.x=7 O=setmetatable({base=B},{__index=D})");
 lua_getglobal(L,"O");
 count_hook_calls=0;
 lua_sethook(L,mutate_on_count,LUA_MASKCOUNT,1);
 G(L)->GCdebt=-1;
 lua_getfield(L,-1,"x");
 assert(lua_tointeger(L,-1)==99 && count_hook_calls>0);
 lua_sethook(L,NULL,0,0);
 lua_settop(L,0);
 puts("PASS count-hook servicepoint equivalence and mutating fallback");
}

/** @brief Own a Lua state for VM-caller and retained-adapter contract tests.
 * Verifies complete callback payload/tag dispatch, guarded and ordinary Lua
 * paths, mutations, false hits and no-result assignment. Assertions fail the
 * test on mismatch; the state is closed after all contracts pass.
 */
void cold_thunk_contracts(){
 lua_State*L=luaL_newstate();luaL_openlibs(L);assert(lua_checkstack(L,64));
 vm_callback_contracts(L);
 count_hook_contracts(L);
 eval(L,"D=function(self,k)return self.base[k]end B={x=7,f=false} O=setmetatable({base=B},{__index=D}) assert(O.x==7)");
 for(int mode=0;mode<4;mode++){
  if(mode==0)lua_getglobal(L,"D");if(mode==1)lua_pushcfunction(L,light);
  if(mode==2){lua_pushinteger(L,43);lua_pushcclosure(L,closure,1);}
  if(mode==3){eval(L,"F=function(self,k) calls=(calls or 0)+1 return 47 end");lua_getglobal(L,"F");}
  lua_getglobal(L,"O");lua_pushstring(L,"x");lua_pushnil(L);
  TValue *function=L->top-4; l_mem debt=G(L)->GCdebt;G(L)->GCdebt=-1;
  const TValue *guard=luaV_delegatebits(L,function->value_,function->tt_,L->top-3,L->top-2);
  G(L)->GCdebt=debt;
  if(mode==0)assert(guard&&ttisnumber(guard)&&nvalue(guard)==z8::fix32(7));else assert(!guard);
  if(mode==0||mode==3){
   G(L)->GCdebt=-1;
   const TValue *typed=luaV_delegateclosure(L,clLvalue(function),L->top-3,L->top-2);
   G(L)->GCdebt=debt;
   if(mode==0)assert(typed&&ttisnumber(typed)&&nvalue(typed)==z8::fix32(7));else assert(!typed);
  }
  invoke(L,1);
  assert(lua_tointeger(L,-1)==(mode==0?7:mode==1?37:mode==2?43:47));lua_settop(L,0);
 }
 eval(L,"B.x=11 setmetatable(O,{__index=D,__add=D}) assert((O+'x')==11)");lua_getglobal(L,"D");lua_getglobal(L,"O");lua_pushstring(L,"x");lua_pushnil(L);invoke(L,1);assert(lua_tointeger(L,-1)==11);lua_settop(L,0);
 lua_getglobal(L,"D");lua_getglobal(L,"O");lua_pushstring(L,"f");lua_pushnil(L);invoke(L,1);assert(lua_isboolean(L,-1)&&!lua_toboolean(L,-1));lua_settop(L,0);
 eval(L,"S=function(self,k,v)self[k]=v end");lua_getglobal(L,"S");lua_getglobal(L,"B");lua_pushstring(L,"x");lua_pushinteger(L,73);invoke(L,0);lua_settop(L,0);eval(L,"assert(B.x==73 and calls==1)");
 lua_close(L);puts("PASS cold thunk payload/tag, Lua/light-C/C-closure fallback, mutation, false and no-result callback");
}
