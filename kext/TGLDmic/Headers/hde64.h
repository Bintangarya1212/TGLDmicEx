#ifndef _HDE64_H_
#define _HDE64_H_

#include <stdint.h>

#pragma pack(push,1)

typedef struct {
    uint8_t len;
    uint8_t p_rep;
    uint8_t p_lock;
    uint8_t p_seg;
    uint8_t p_66;
    uint8_t p_67;
    uint8_t rex;
    uint8_t rex_rn;
    uint8_t rex_b;
    uint8_t rex_x;
    uint8_t rex_r;
    uint8_t rex_w;
    uint8_t opcode;
    uint8_t opcode2;
    uint8_t modrm;
    uint8_t modrm_mod;
    uint8_t modrm_reg;
    uint8_t modrm_rm;
    uint8_t sib;
    uint8_t sib_scale;
    uint8_t sib_index;
    uint8_t sib_base;
    union {
        uint8_t d8;
        uint16_t d16;
        uint32_t d32;
    } disp;
    union {
        uint8_t i8;
        uint16_t i16;
        uint32_t i32;
        uint64_t i64;
    } imm;
    uint32_t flags;
} hde64s;

#pragma pack(pop)

#ifdef __cplusplus
extern "C" {
#endif

unsigned int hde64_disasm(const void *code, hde64s *hs);

#ifdef __cplusplus
}
#endif

#endif // _HDE64_H_
