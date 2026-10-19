/* Keep the measured iterator/lookup working set together independently of
 * LTO object emission order. Use compiler function sections and their natural
 * alignment; do not pin addresses or retain otherwise-unused functions. */
SECTIONS {
  .p8p_vm_hot : {
    KEEP(*(.text.start))
    HIDDEN(__p8p_hot_all_next_start = .);
    *(.text._ZL12api_all_nextP9lua_State*)
    ASSERT(. > __p8p_hot_all_next_start, "Missing hot all_next section; recheck VM layout");
    HIDDEN(__p8p_hot_fastchain_start = .);
    *(.text._ZL9fastchainP9lua_StatePK10lua_TValueS3_*)
    ASSERT(. > __p8p_hot_fastchain_start, "Missing hot fastchain section; recheck VM layout");
    HIDDEN(__p8p_hot_gettable_start = .);
    *(.text._Z13luaV_gettableP9lua_StatePK10lua_TValuePS1_S4_*)
    ASSERT(. > __p8p_hot_gettable_start, "Missing hot gettable section; recheck VM layout");
    HIDDEN(__p8p_hot_foreach_start = .);
    *(.text._ZL11api_foreachP9lua_State*)
    ASSERT(. > __p8p_hot_foreach_start, "Missing hot foreach section; recheck VM layout");
    HIDDEN(__p8p_hot_icache_miss_start = .);
    *(.text._ZL11icache_missP5TableP7TStringPj*)
    ASSERT(. > __p8p_hot_icache_miss_start, "Missing hot icache_miss section; recheck VM layout");
  } > SDRAM
}
INSERT BEFORE .text;
