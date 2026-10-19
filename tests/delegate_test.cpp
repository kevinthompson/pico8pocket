#include <lauxlib.h>
#include <lualib.h>
#include <lstate.h>
#include <lgc.h>
#include <lopcodes.h>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <lvm.h>
static int failures;
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);++failures;}}while(0)
/** @brief Run fixture code and restore the caller's original Lua stack top.
 * Unexpected load/runtime errors print a diagnostic and exit with status 2.
 */
static void eval(lua_State*L,const char*code){int n=lua_gettop(L);int rc=luaL_loadbuffer(L,code,strlen(code),"@guarded-fixture")||lua_pcall(L,0,LUA_MULTRET,0);if(rc){fprintf(stderr,"unexpected Lua error: %s\n",lua_tostring(L,-1));exit(2);}lua_settop(L,n);}
/** @brief Probe delegate D with O and key for guarded resolution or fallback.
 * Temporarily clears GC debt to isolate other guards, then restores debt and
 * stack height. Returns a copy in static scratch storage, overwritten by the
 * next probe; this storage is not a GC root and must be consumed immediately.
 */
static const TValue* probe(lua_State*L,const char*key,bool fallback){
    int n=lua_gettop(L);lua_getglobal(L,"O");lua_pushstring(L,key);lua_getglobal(L,"D");
    l_mem debt=G(L)->GCdebt;G(L)->GCdebt=-1;
    const TValue* result=luaV_delegateget(L,L->top-3,L->top-2,L->top-1);
    G(L)->GCdebt=debt;CHECK((result==NULL)==fallback);
    static TValue value;if(result)setobj(L,&value,result);lua_settop(L,n);return result?&value:NULL;
}
/** @brief Require a guarded numeric hit equal to the expected fixed value. */
static void number(lua_State*L,const char*key,int expected){const TValue*v=probe(L,key,false);CHECK(v&&ttisnumber(v)&&nvalue(v)==z8::fix32(expected));}
/** @brief Do nothing when tests install a hook to isolate hook-mask eligibility. */
static void hook(lua_State*,lua_Debug*){}
void cold_thunk_contracts();
/** @brief Run guard and callback contracts in isolated Lua states.
 * Covers mutation, false values, depth/capacity limits, hooks, GC debt, hash
 * collisions and observable fallback behavior including errors and yields.
 * Closes the owned state and returns nonzero if any CHECK assertion failed.
 */
int main(){
    cold_thunk_contracts();
    lua_State*L=luaL_newstate();CHECK(L);luaL_openlibs(L);
    eval(L,"D=function(self,k) return self.base[k] end B={x=7,f=false} O=setmetatable({base=B},{__index=D})");
    lua_getglobal(L,"D");
    lua_pop(L,1);
    probe(L,"x",true); // Cold prototype must retain normal initialization.
    eval(L,"assert(O.x==7)");number(L,"x",7);
    eval(L,"B.x=9");number(L,"x",9);
    eval(L,"O.base={x=13,f=false}");number(L,"x",13);
    const TValue*f=probe(L,"f",false);CHECK(f&&ttisboolean(f)&&!bvalue(f));
    probe(L,"absent",true);
    eval(L,"O.x=99 assert(O.x==99) O.x=nil assert(O.x==13)");
    eval(L,"B={x=21} O.base=setmetatable({base=B},{__index=D}) assert(O.x==21)");number(L,"x",21);
    eval(L,"B.x=23");number(L,"x",23);
    eval(L,"local t={x=29} for i=1,4 do t=setmetatable({base=t},{__index=D}) end O=t assert(O.x==29)");number(L,"x",29);
    eval(L,"local t={x=31} for i=1,5 do t=setmetatable({base=t},{__index=D}) end O=t assert(O.x==31)");probe(L,"x",true);
    eval(L,"O=setmetatable({base={x=7}},{__index=D}) assert(O.x==7)");
    for(int mask:{LUA_MASKCALL,LUA_MASKRET,LUA_MASKLINE}){lua_sethook(L,hook,mask,1);probe(L,"x",true);lua_sethook(L,NULL,0,0);}
    lua_sethook(L,hook,LUA_MASKCOUNT,1);number(L,"x",7);lua_sethook(L,NULL,0,0);
    lua_getglobal(L,"O");lua_pushliteral(L,"x");lua_getglobal(L,"D");
    l_mem debt=G(L)->GCdebt;G(L)->GCdebt=1;CHECK(!luaV_delegateget(L,L->top-3,L->top-2,L->top-1));G(L)->GCdebt=debt;
    unsigned short calls=L->nCcalls;L->nCcalls=199;CHECK(!luaV_delegateget(L,L->top-3,L->top-2,L->top-1));L->nCcalls=calls;
    StkId last=L->stack_last;L->stack_last=L->top+1;CHECK(!luaV_delegateget(L,L->top-3,L->top-2,L->top-1));L->stack_last=last;lua_pop(L,3);
    lua_getglobal(L,"O");lua_pushliteral(L,"x");lua_getglobal(L,"D");
    debt=G(L)->GCdebt;G(L)->GCdebt=-1;CallInfo *next=L->ci->next;L->ci->next=NULL;
    CHECK(!luaV_delegateget(L,L->top-3,L->top-2,L->top-1));L->ci->next=next;G(L)->GCdebt=debt;lua_pop(L,3);
    const char*rejects[]={"function(s,k) return s.base[k]+0 end","function(s,k) local v=s.base[k] side=side+1 return v end","function(s,k,...) return s.base[k] end","function(s,k) return s[k] end","function(s,k) return 3 end"};
    for(const char*body:rejects){char code[512];snprintf(code,sizeof(code),"side=0 D=%s O=setmetatable({base={x=7}},{__index=D})",body);eval(L,code);eval(L,"pcall(function() return O.x end)");probe(L,"x",true);}
    eval(L,"D=function(self,k) return self.base[k] end O=setmetatable({base=3},{__index=D})");probe(L,"x",true);
    eval(L,"O=setmetatable({base={}},{__index=D}) setmetatable(O.base,{__index=function(t,k) side=side+1 return 41 end}) assert(O.x==41) assert(side==1)");probe(L,"x",true);
    eval(L,"O.base=setmetatable({},{__index=function(t,k) coroutine.yield(43) return 47 end}) local c=coroutine.create(function() return O.x end) local ok,x=coroutine.resume(c) assert(ok and x==43) ok,x=coroutine.resume(c) assert(ok and x==47)");probe(L,"x",true);
    eval(L,"O=setmetatable({},{__index=D}) O.base=O local ok,msg=pcall(function() return O.x end) assert(not ok and type(msg)=='string')");probe(L,"x",true);
    // Exercise private raw probes across populated collision chains, removed
    // entries, an empty/dummy table and a changed metatable after a cached miss.
    eval(L,"D=function(self,k)return self.base[k]end B={} for i=1,1024 do B['key'..i]=i end O=setmetatable({base=B},{__index=D}) assert(O.key1==1)");
    for(int i=1;i<=1024;i++){char key[32];snprintf(key,sizeof(key),"key%d",i);number(L,key,i);}
    eval(L,"for i=1,1024,2 do B['key'..i]=nil end");
    for(int i=1;i<=1024;i++){char key[32];snprintf(key,sizeof(key),"key%d",i);if(i%2)probe(L,key,true);else number(L,key,i);}
    eval(L,"O.base={} assert(O.missing==nil)");probe(L,"missing",true);
    eval(L,"B=setmetatable({}, {}) O.base=B assert(O.x==nil)");probe(L,"x",true);
    eval(L,"setmetatable(B,{__index=D}) B.base={x=97} assert(O.x==97)");number(L,"x",97);
    lua_close(L);printf("delegate contracts: %d failures\n",failures);return failures?1:0;
}
