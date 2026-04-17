#ifndef GFX_VM_H
#define GFX_VM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_VM_REG_COUNT 16
#define GFX_VM_MAX_PROGS 16
#define GFX_VM_MAX_BYTECODE_SIZE 384
#define GFX_VM_MAX_STRTABLE_SIZE 128

// Protocol-level sentinel for "no prog id allocated" (same value as
// FMRB_GFX_VM_INVALID_PROG_ID in fmrb_link_protocol.h).
#define GFX_VM_INVALID_PROG_ID 0xFF

// Opcodes (must match Ruby-side bytecode compiler)
#define GFX_VM_OP_END             0x00
#define GFX_VM_OP_CLEAR           0x01
#define GFX_VM_OP_FILL_RECT       0x02
#define GFX_VM_OP_DRAW_RECT       0x03
#define GFX_VM_OP_FILL_ROUND_RECT 0x04
#define GFX_VM_OP_DRAW_ROUND_RECT 0x05
#define GFX_VM_OP_DRAW_LINE       0x06
#define GFX_VM_OP_FILL_CIRCLE     0x07
#define GFX_VM_OP_DRAW_TEXT       0x08

// Operand encoding:
//   bit15 = 0: immediate (15-bit signed, -16384..+16383)
//   bit15 = 1: register reference (bits 3-0 = reg_id)
#define GFX_VM_OPERAND_IS_REG(w)   (((w) & 0x8000) != 0)
#define GFX_VM_OPERAND_REG_ID(w)   ((w) & 0x0F)
#define GFX_VM_OPERAND_IMM(w)      ((int16_t)(((int16_t)((w) << 1)) >> 1))

// Initialize VM state (called once at boot).
void gfx_vm_init(void);

// Register a new program. Copies bytecode and strtable into internal storage.
// Returns an allocated prog_id in [0..GFX_VM_MAX_PROGS-1], or
// GFX_VM_INVALID_PROG_ID when the pool is full or the payload is too large.
uint8_t gfx_vm_define_prog(uint16_t canvas_id,
                           const uint8_t *bytecode, uint16_t bytecode_len,
                           const uint8_t *strtable, uint16_t strtable_len);

// Apply register updates and run the program on the given target sprite.
// reg_updates: packed as [uint8_t reg_id, int16_t value] * reg_count
// Returns 0 on success, -1 on error.
int gfx_vm_exec_prog(uint16_t canvas_id, uint8_t prog_id,
                     const uint8_t *reg_updates, uint8_t reg_count,
                     void *target_sprite);

// Release the slot. Safe to call on an already-free slot.
int gfx_vm_delete_prog(uint8_t prog_id);

// Free all programs associated with canvas_id (used on canvas delete).
void gfx_vm_delete_progs_by_canvas(uint16_t canvas_id);

#ifdef __cplusplus
}
#endif

#endif // GFX_VM_H
