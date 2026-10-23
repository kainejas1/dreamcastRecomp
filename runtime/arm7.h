#ifndef DC_RUNTIME_ARM7_H
#define DC_RUNTIME_ARM7_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct arm7_bus {
    uint8_t (*read8)(uint32_t address, void *user);
    uint32_t (*read32)(uint32_t address, void *user);
    void (*write8)(uint32_t address, uint8_t value, void *user);
    void (*write32)(uint32_t address, uint32_t value, void *user);
    void *user;
} arm7_bus;
void arm7_set_bus(const arm7_bus *bus);
void arm7_reset(void);
int arm7_run(int n);
void arm7_fiq(int level);
uint32_t arm7_reg(int r);
uint32_t arm7_cpsr(void);
#ifdef __cplusplus
}
#endif
#endif
