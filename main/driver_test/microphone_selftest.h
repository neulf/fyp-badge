#pragma once
#include "sdkconfig.h"
#include "lvgl.h"

#ifdef CONFIG_TEMPLATE_MIC_SELFTEST
void microphone_selftest_start(lv_obj_t *button);
#else
static inline void microphone_selftest_start(lv_obj_t *button) { (void)button; }
#endif
