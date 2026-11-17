/* Adapt the shared ARM64 MMIO decoder to Bionic's signal context.
 * The original macOS handler is in the pinned TSFP PC port, GPL-3.0-only.
 */
#include "arm64_access.h"
#include "mmio_device.h"

#include <signal.h>
#include <ucontext.h>

int tsfp_android_mmio(siginfo_t *info, void *context)
{
    uintptr_t host = (uintptr_t)info->si_addr;
    uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    if (!base || host < base || host - base > UINT32_MAX)
        return 0;

    uint32_t address = (uint32_t)(host - base);
    if (!(address >= 0xfe800000u && address < 0xfe830000u) &&
        !(address >= 0xfd600000u && address < 0xfd604000u) &&
        !(address >= 0xfec00000u && address < 0xfec04000u))
        return 0;

    mcontext_t *cpu = &((ucontext_t *)context)->uc_mcontext;
    uint64_t regs[32];
    for (unsigned i = 0; i < 31; i++)
        regs[i] = cpu->regs[i];
    regs[31] = cpu->sp;

    uint32_t instruction;
    memcpy(&instruction, (const void *)(uintptr_t)cpu->pc, sizeof instruction);
    NativeAccess access;
    if (!native_decode_access(instruction, regs, &access) || access.address != host)
        return 0;

    uint64_t value = access.rt == 31 ? 0 : regs[access.rt];
    if (access.bytes < 8)
        value &= (UINT64_C(1) << (access.bytes * 8)) - 1;
    if (!tsfp_mmio_device(host, access.bytes, access.load, &value))
        return 0;

    if (access.load && access.rt != 31) {
        if (access.sign_extend) {
            unsigned shift = 64 - access.bytes * 8;
            value = (uint64_t)((int64_t)(value << shift) >> shift);
        }
        cpu->regs[access.rt] = access.result64 ? value : (uint32_t)value;
    }
    if (access.update_base) {
        if (access.rn == 31)
            cpu->sp = access.writeback;
        else
            cpu->regs[access.rn] = access.writeback;
    }
    cpu->pc += sizeof instruction;
    return 1;
}
