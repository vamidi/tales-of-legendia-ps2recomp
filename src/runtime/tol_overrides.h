#pragma once

class PS2Runtime;

namespace tol
{
    // Installs native overrides for guest system code that has no host counterpart - the IOP/SIF services the
    // game drives directly instead of through the HLE stubs.
    //
    // Call after loadELF(): the function table must be populated before replaceFunction() can find a slot, and
    // loadELF() also rewrites the IO paths that later overrides may need.
    void installOverrides(PS2Runtime &runtime);
}
