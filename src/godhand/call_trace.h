#pragma once

// Guest call tracing and memory watches. GENERIC: the Urban Reign port's src/urban/call_trace.h with the URBAN_* env
// names renamed GODHAND_* and Urban's CRI PSM audit (URBAN_PSM_AUDIT, fixed Urban Reign addresses) removed.

class PS2Runtime;

namespace godhand::calltrace
{
    // GODHAND_CALL_TRACE=0x394dd8,0x2ae4a8,...: log every entry into those guest functions ($a0-$a3, $s0-$s3, $ra, $sp).
    // GODHAND_WATCH=0x34d398:232[,...]: on each traced entry also print the byte at *(u32*)0x34d398 + 232 ("ptr:offset"
    // dereferences ptr; "=addr" prints the byte at addr itself).
    // GODHAND_WATCH_POLL=0x34d398:232: a host thread polls that byte continuously and logs each change together with
    // the live guest PC (generated code stores ctx->pc before every instruction, so it points at or near the writer).
    // GODHAND_HW_WATCH=0x34d308[,len]: x64 debug-register write watchpoint on that guest address (len 1/2/4/8), armed on
    // the GameThread. Each hit logs the exact guest PC and the new value. GODHAND_HW_WATCH_RW=1 watches reads too;
    // GODHAND_HW_WATCH_STACK=1 adds the native stack. Windows only.
    void installFromEnvironment(PS2Runtime &runtime);
}
