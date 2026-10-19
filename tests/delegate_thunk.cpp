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
static int light(lua_State*L){lua_pushinteger(L,37);return 1;}
static int closure(lua_State*L){lua_pushvalue(L,lua_upvalueindex(1));return 1;}
static void eval(lua_State*L,const char*s){int rc=luaL_loadstring(L,s)||lua_pcall(L,0,0,0);if(rc){fprintf(stderr,"%s\n",lua_tostring(L,-1));assert(false);}}
static void invoke(lua_State*L,int hasres){
 static_assert(sizeof(Value)==sizeof(uintptr_t),"function payload ABI");
 TValue*f=L->top-4,*t=L->top-3,*k=L->top-2,*v=L->top-1;luaV_callindex(L,f->value_,f->tt_,t,k,v,hasres);
}
void cold_thunk_contracts(){
 lua_State*L=luaL_newstate();luaL_openlibs(L);assert(lua_checkstack(L,64));
 eval(L,"D=function(self,k)return self.base[k]end B={x=7,f=false} O=setmetatable({base=B},{__index=D}) assert(O.x==7)");
 for(int mode=0;mode<4;mode++){
  if(mode==0)lua_getglobal(L,"D");if(mode==1)lua_pushcfunction(L,light);
  if(mode==2){lua_pushinteger(L,43);lua_pushcclosure(L,closure,1);}
  if(mode==3){eval(L,"F=function(self,k) calls=(calls or 0)+1 return 47 end");lua_getglobal(L,"F");}
  lua_getglobal(L,"O");lua_pushstring(L,"x");lua_pushnil(L);invoke(L,1);
  assert(lua_tointeger(L,-1)==(mode==0?7:mode==1?37:mode==2?43:47));lua_settop(L,0);
 }
 eval(L,"B.x=11");lua_getglobal(L,"D");lua_getglobal(L,"O");lua_pushstring(L,"x");lua_pushnil(L);invoke(L,1);assert(lua_tointeger(L,-1)==11);lua_settop(L,0);
 lua_getglobal(L,"D");lua_getglobal(L,"O");lua_pushstring(L,"f");lua_pushnil(L);invoke(L,1);assert(lua_isboolean(L,-1)&&!lua_toboolean(L,-1));lua_settop(L,0);
 eval(L,"S=function(self,k,v)self[k]=v end");lua_getglobal(L,"S");lua_getglobal(L,"B");lua_pushstring(L,"x");lua_pushinteger(L,73);invoke(L,0);lua_settop(L,0);eval(L,"assert(B.x==73 and calls==1)");
 lua_close(L);puts("PASS cold thunk payload/tag, Lua/light-C/C-closure fallback, mutation, false and no-result callback");
}
