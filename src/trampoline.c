#include "trampoline.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#if __APPLE__
#include <mach-o/dyld.h>
#endif
#include "machine.h"

#define PAGE_ALIGNED(a,size) (void*)(((uintptr_t)a) & ~0xfff) 

static
int g_patched = 0;

static char * g_trampoline = NULL;
// the machine code size copied over from the callee
static int g_trampoline_length;

void _jmp_to(char * a, uintptr_t addr) {
    a[0] = 0x48; // REX.W
    a[1] = 0xb8;
 
    // little endian
    a[2] = addr & 0xff;
    a[3] = (addr >> 8) & 0xff;
    a[4] = (addr >> 16) & 0xff;
    a[5] = (addr >> 24) & 0xff;
    a[6] = (addr >> 32) & 0xff;
    a[7] = (addr >> 40) & 0xff;
    a[8] = (addr >> 48) & 0xff;
    a[9] = (addr >> 56) & 0xff;

    a[10] = 0xff;
    a[11] = 0xe0;
}

// a hilarious typo, tramp -> trump :)
int _redirect_trampoline_and_back(char * callee, char * trump) {

    char * trump_first_byte = trump;
    int needed_bytes = 12;
    int bytes = 0;
    char * ptr = callee;

    // 1) copy the instructions that should be redone in the trampoline
    while (bytes < needed_bytes) {
        int res = vmp_machine_code_instr_length(ptr);
        if (res == 0) {
            return 1;
        }
        bytes += res;
        ptr+=res;
    }
    bytes = g_trampoline_length;
    (void)memcpy(trump, callee, bytes);

    // 2) custom instructions needed
    {
        // MOV %rdi, %rbx (rbx a caller saved register)
        trump[bytes+0] = 0x48;
        trump[bytes+1] = 0x89;
        trump[bytes+2] = 0xfb;

        // JMP back to callee + byes
        _jmp_to(trump+bytes+3, (uintptr_t)callee+bytes);
    }

    // 3) overwrite the first few bytes of callee to jump to tramp
    // callee must jump to begining of trump
    _jmp_to(callee, (uintptr_t)trump_first_byte);

    return 0;
}


int vmp_patch_callee_trampoline(const char * callee_name)
{
    void ** callee_addr = (void**)dlsym(RTLD_DEFAULT, callee_name);
    int result;
    int pagesize = sysconf(_SC_PAGESIZE);
    errno = 0;

    result = mprotect(PAGE_ALIGNED(callee_addr, pagesize), pagesize*2, PROT_READ|PROT_WRITE);
    if (result != 0) {
        dprintf(2, "read|write protecting callee_addr\n");
        return -1;
    }
    // create a new page and set it all of it writable
    char * page = (char*)mmap(NULL, pagesize, PROT_READ|PROT_WRITE|PROT_EXEC,
                              MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
    if (page == NULL) {
        return -1;
    }

    char * a = (char*)callee_addr;
    if (_redirect_trampoline_and_back(a, page) != 0) {
        return -1;
    }

    result = mprotect(PAGE_ALIGNED(callee_addr, pagesize), pagesize*2, PROT_READ|PROT_EXEC);
    if (result != 0) {
        dprintf(2, "read|exec protecting callee addr\n");
        return -1;
    }
    // revert, the page should not be writable any more now!
    result = mprotect((void*)page, pagesize, PROT_READ|PROT_EXEC);
    if (result != 0) {
        dprintf(2, "read|exec protecting tramp\n");
        return -1;
    }

    g_trampoline = page;

    return 0;
}

int vmp_unpatch_callee_trampoline(const char * callee_name)
{
    if (!g_patched) {
        return -1;
    }

    void ** callee_addr = (void**)dlsym(RTLD_DEFAULT, callee_name);
    int result;
    int pagesize = sysconf(_SC_PAGESIZE);
    errno = 0;

    result = mprotect(PAGE_ALIGNED(callee_addr, pagesize), pagesize*2, PROT_READ|PROT_WRITE);
    if (result != 0) {
        dprintf(2, "read|write protecting callee_addr\n");
        return 1;
    }

    // copy back as if nothing ever happened!!
    (void)memcpy(callee_addr, g_trampoline, g_trampoline_length);

    result = mprotect(PAGE_ALIGNED(callee_addr, pagesize), pagesize*2, PROT_READ|PROT_EXEC);
    if (result != 0) {
        dprintf(2, "read|exec protecting callee addr\n");
        return 1;
    }

    munmap(g_trampoline, pagesize);
    g_trampoline = NULL;
    g_trampoline_length = 0;

    return 0;
}
