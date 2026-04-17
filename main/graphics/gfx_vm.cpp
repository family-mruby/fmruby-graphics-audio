#include <cstring>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

extern "C" {
#include "gfx_vm.h"
#include "esp_log.h"
}

static const char *TAG = "gfx_vm";

typedef struct {
    bool active;
    uint16_t canvas_id;
    uint16_t bytecode_len;
    uint16_t strtable_len;
    uint8_t bytecode[GFX_VM_MAX_BYTECODE_SIZE];
    uint8_t strtable[GFX_VM_MAX_STRTABLE_SIZE];
    int16_t regs[GFX_VM_REG_COUNT];
} prog_definition_t;

static prog_definition_t g_progs[GFX_VM_MAX_PROGS];

extern "C" void gfx_vm_init(void) {
    memset(g_progs, 0, sizeof(g_progs));
    ESP_LOGI(TAG, "gfx_vm initialized: max_progs=%d, max_bc=%d, max_st=%d",
             GFX_VM_MAX_PROGS, GFX_VM_MAX_BYTECODE_SIZE, GFX_VM_MAX_STRTABLE_SIZE);
}

extern "C" uint8_t gfx_vm_define_prog(uint16_t canvas_id,
                                      const uint8_t *bytecode, uint16_t bytecode_len,
                                      const uint8_t *strtable, uint16_t strtable_len) {
    if (bytecode_len > GFX_VM_MAX_BYTECODE_SIZE) {
        ESP_LOGE(TAG, "DEFINE_PROG: bytecode_len=%u exceeds max=%d",
                 bytecode_len, GFX_VM_MAX_BYTECODE_SIZE);
        return GFX_VM_INVALID_PROG_ID;
    }
    if (strtable_len > GFX_VM_MAX_STRTABLE_SIZE) {
        ESP_LOGE(TAG, "DEFINE_PROG: strtable_len=%u exceeds max=%d",
                 strtable_len, GFX_VM_MAX_STRTABLE_SIZE);
        return GFX_VM_INVALID_PROG_ID;
    }

    for (uint8_t i = 0; i < GFX_VM_MAX_PROGS; i++) {
        if (!g_progs[i].active) {
            prog_definition_t *p = &g_progs[i];
            p->active = true;
            p->canvas_id = canvas_id;
            p->bytecode_len = bytecode_len;
            p->strtable_len = strtable_len;
            if (bytecode_len > 0 && bytecode) {
                memcpy(p->bytecode, bytecode, bytecode_len);
            }
            if (strtable_len > 0 && strtable) {
                memcpy(p->strtable, strtable, strtable_len);
            }
            memset(p->regs, 0, sizeof(p->regs));
            ESP_LOGI(TAG, "DEFINE_PROG ok: prog_id=%u canvas=%u bc=%u st=%u",
                     i, canvas_id, bytecode_len, strtable_len);
            return i;
        }
    }
    ESP_LOGE(TAG, "DEFINE_PROG: pool full");
    return GFX_VM_INVALID_PROG_ID;
}

extern "C" int gfx_vm_delete_prog(uint8_t prog_id) {
    if (prog_id >= GFX_VM_MAX_PROGS) return -1;
    if (!g_progs[prog_id].active) return 0;
    g_progs[prog_id].active = false;
    ESP_LOGI(TAG, "DELETE_PROG: prog_id=%u", prog_id);
    return 0;
}

extern "C" void gfx_vm_delete_progs_by_canvas(uint16_t canvas_id) {
    for (uint8_t i = 0; i < GFX_VM_MAX_PROGS; i++) {
        if (g_progs[i].active && g_progs[i].canvas_id == canvas_id) {
            g_progs[i].active = false;
            ESP_LOGI(TAG, "DELETE_PROG (by canvas): prog_id=%u canvas=%u", i, canvas_id);
        }
    }
}

// Look up a string from strtable by id. strtable format:
//   [len(1)] [chars(len)] [len(1)] [chars(len)] ...
// Returns pointer to start of string and length via out_len. Returns NULL if not found.
static const char *lookup_strtable(const prog_definition_t *p, uint16_t str_id, uint8_t *out_len) {
    uint16_t pos = 0;
    uint16_t idx = 0;
    while (pos < p->strtable_len) {
        uint8_t len = p->strtable[pos++];
        if (pos + len > p->strtable_len) return NULL;
        if (idx == str_id) {
            *out_len = len;
            return (const char *)&p->strtable[pos];
        }
        pos += len;
        idx++;
    }
    return NULL;
}

static void vm_execute(prog_definition_t *prog, LGFX_Sprite *target) {
    const uint8_t *pc = prog->bytecode;
    const uint8_t *end = pc + prog->bytecode_len;

    while (pc < end) {
        uint8_t opcode = *pc++;
        if (pc >= end) break;
        uint8_t nops = *pc++;

        int16_t args[8];
        if (nops > 8) {
            ESP_LOGE(TAG, "vm_execute: nops=%u exceeds max 8", nops);
            return;
        }
        if (pc + nops * 2 > end) {
            ESP_LOGE(TAG, "vm_execute: truncated operand stream");
            return;
        }
        for (uint8_t i = 0; i < nops; i++) {
            uint16_t w = (uint16_t)pc[0] | ((uint16_t)pc[1] << 8);
            pc += 2;
            if (GFX_VM_OPERAND_IS_REG(w)) {
                args[i] = prog->regs[GFX_VM_OPERAND_REG_ID(w)];
            } else {
                args[i] = GFX_VM_OPERAND_IMM(w);
            }
        }

        switch (opcode) {
            case GFX_VM_OP_END:
                return;
            case GFX_VM_OP_CLEAR:
                target->fillScreen((uint8_t)args[0]);
                break;
            case GFX_VM_OP_FILL_RECT:
                target->fillRect(args[0], args[1], args[2], args[3], (uint8_t)args[4]);
                break;
            case GFX_VM_OP_DRAW_RECT:
                target->drawRect(args[0], args[1], args[2], args[3], (uint8_t)args[4]);
                break;
            case GFX_VM_OP_FILL_ROUND_RECT:
                target->fillRoundRect(args[0], args[1], args[2], args[3], args[4], (uint8_t)args[5]);
                break;
            case GFX_VM_OP_DRAW_ROUND_RECT:
                target->drawRoundRect(args[0], args[1], args[2], args[3], args[4], (uint8_t)args[5]);
                break;
            case GFX_VM_OP_DRAW_LINE:
                target->drawLine(args[0], args[1], args[2], args[3], (uint8_t)args[4]);
                break;
            case GFX_VM_OP_FILL_CIRCLE:
                target->fillCircle(args[0], args[1], args[2], (uint8_t)args[3]);
                break;
            case GFX_VM_OP_DRAW_TEXT: {
                // args: x, y, color, str_id
                uint8_t str_len = 0;
                const char *str = lookup_strtable(prog, (uint16_t)args[3], &str_len);
                if (str) {
                    char buf[64];
                    if (str_len >= sizeof(buf)) str_len = sizeof(buf) - 1;
                    memcpy(buf, str, str_len);
                    buf[str_len] = '\0';
                    target->setTextColor((uint8_t)args[2]);
                    target->setCursor(args[0], args[1]);
                    target->print(buf);
                }
                break;
            }
            default:
                ESP_LOGW(TAG, "vm_execute: unknown opcode 0x%02X", opcode);
                return;
        }
    }
}

extern "C" int gfx_vm_exec_prog(uint16_t canvas_id, uint8_t prog_id,
                                const uint8_t *reg_updates, uint8_t reg_count,
                                void *target_sprite) {
    if (prog_id >= GFX_VM_MAX_PROGS) {
        ESP_LOGE(TAG, "EXEC_PROG: invalid prog_id=%u", prog_id);
        return -1;
    }
    prog_definition_t *p = &g_progs[prog_id];
    if (!p->active) {
        ESP_LOGE(TAG, "EXEC_PROG: prog_id=%u not defined", prog_id);
        return -1;
    }
    if (p->canvas_id != canvas_id) {
        ESP_LOGW(TAG, "EXEC_PROG: canvas mismatch (expected=%u got=%u)", p->canvas_id, canvas_id);
    }
    if (!target_sprite) {
        ESP_LOGE(TAG, "EXEC_PROG: null target sprite");
        return -1;
    }

    // Apply register updates
    for (uint8_t i = 0; i < reg_count; i++) {
        uint8_t reg_id = reg_updates[i * 3 + 0];
        int16_t val = (int16_t)((uint16_t)reg_updates[i * 3 + 1]
                                | ((uint16_t)reg_updates[i * 3 + 2] << 8));
        if (reg_id < GFX_VM_REG_COUNT) {
            p->regs[reg_id] = val;
        }
    }

    vm_execute(p, (LGFX_Sprite *)target_sprite);
    return 0;
}
