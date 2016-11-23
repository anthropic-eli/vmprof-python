#pragma once

/**
 * This whole setup is very strange. There was just one C file called
 * _vmprof.c which included all *.h files to copy code. Unsure what
 * the goal was with this design, but I assume it just 'GREW'
 *
 * Thus I'm (plan_rich) slowly trying to separate this. *.h files
 * should not have complex implementations (all of them currently have them)
 */

#include <inttypes.h>

#define SINGLE_BUF_SIZE (8192 - 2 * sizeof(unsigned int))

#define ROUTINE_IS_PYTHON(RIP) ((unsigned long long)RIP & 0x1) == 0
#define ROUTINE_IS_C(RIP) ((unsigned long long)RIP & 0x1) == 1

typedef uint64_t ptr_t;

/* This returns the address of the code object
   as the identifier.  The mapping from identifiers to string
   representations of the code object is done elsewhere, namely:

   * If the code object dies while vmprof is enabled,
     PyCode_Type.tp_dealloc will emit it.  (We don't handle nicely
     for now the case where several code objects are created and die
     at the same memory address.)

   * When _vmprof.disable() is called, then we look around the
     process for code objects and emit all the ones that we can
     find (which we hope is very close to 100% of them).
*/
#define CODE_ADDR_TO_UID(co)  (((unsigned long)(co)))

#define CPYTHON_HAS_FRAME_EVALUATION PY_VERSION_HEX >= 0x30600B0

PyObject* cpython_vmprof_PyEval_EvalFrameEx(PyFrameObject *f, int throwflag);
