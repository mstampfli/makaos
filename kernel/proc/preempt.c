// preempt_disable / preempt_enable / preempt_disabled are ALWAYS_INLINE
// in kernel/include/preempt.h so every call site compiles to literal
// machine instructions.  No out-of-line code is needed here; the file is
// kept in the build tree as a documentation anchor and to preserve the
// symbol path for any future out-of-line slow paths we might add.
#include "preempt.h"
