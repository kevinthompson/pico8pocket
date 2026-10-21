# Runtime version-1 fixture

`runtime-v1.state.z` is a zlib-compressed state written by the original runtime
at commit `cbd2470`, before the offline handoff wrappers and state version 2.
It contains only this project-owned cartridge (zero-filled ROM):

```lua
native=load
compiled=load('return 31')
value=compiled()
after=0
function _update() after+=1 end
```

The writer used `p8p_runtime_load`, then `p8p_runtime_save_state`, without stepping
the cartridge. The uncompressed state is 82,709 bytes. `make test` decompresses
it into the ignored build directory and tests genuine old permanent-function
references and version-1 loading, alongside generated version-2 round trips.
No supplied game code, game assets or PICO-8 executable are included.

`runtime-v1-aliases.state.z` was independently generated with that same baseline
runtime, also without stepping, using this project-owned source:

```lua
resume_alias=coresume
protected_alias=pcall
xprotected_alias=xpcall
after=0
function _update()
  resume_alias(cocreate(function() load('#target','parent','hello') end))
  after=77
end
```

Its uncompressed size is 82,854 bytes. Tests restore it separately to exercise
all three old aliases, including native callbacks and xpcall error handlers.
This checks the version-1 alias migration against real old permanent IDs.
