#pragma once

// Guest call counts. GENERIC: the Urban Reign port's src/urban/call_profile.h (URBAN_* env names -> GODHAND_*).

class PS2Runtime;

namespace godhand::callprofile
{
    // GODHAND_CALL_PROFILE=1: count calls to every recompiled function entry (wraps the dispatch table slots, so it
    // must run after all other bindings). Costs one extra indirect call per guest call.
    void installFromEnvironment(PS2Runtime &runtime);

    // Prints the most called guest functions since the previous report (no-op when profiling is off).
    // GODHAND_CALL_PROFILE_RANGE=<lo>-<hi> (hex) also lists every function called in that range.
    void report(int top);
}
