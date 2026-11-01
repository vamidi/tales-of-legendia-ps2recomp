#pragma once

struct R5900Context;
class PS2Runtime;

namespace tol {
void repairVuBoundsFlags(R5900Context &ctx, unsigned comparison);
bool installVuBoundsComparisons(PS2Runtime &runtime);
}
