#pragma once
#include <stdint.h>
void mngs_clk_dump(void (*kv)(const char *, uint64_t), void (*out)(const char *), const char *tag);
void mngs_pll_try(void (*kv)(const char *, uint64_t), void (*out)(const char *));
/* boot-arg mngsexp=1|2 (log prefix "MEXP:") */
void mngs_exp1(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void));
int mngs_switch_clk(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void));
void mngs_exp_restore(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void));
