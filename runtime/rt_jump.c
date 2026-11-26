#include "arm_rt.h"

__attribute__((noreturn, noinline)) void rt_longjmp_host(void **jb)
{
    __builtin_longjmp(jb, 1);
}
