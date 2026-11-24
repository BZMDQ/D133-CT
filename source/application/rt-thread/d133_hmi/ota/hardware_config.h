#ifndef D133_HARDWARE_CONFIG_H
#define D133_HARDWARE_CONFIG_H

#include "display_profile_manager.h"

typedef struct {
    char code[DISPLAY_PROFILE_CODE_LENGTH];
    char slot[DISPLAY_PROFILE_SLOT_LENGTH];
    int custom;
} hardware_config_profile_t;

typedef enum {
    HARDWARE_CONFIG_OK = 0,
    HARDWARE_CONFIG_INVALID = 4,
    HARDWARE_CONFIG_SIGNATURE = 9,
    HARDWARE_CONFIG_UNSUPPORTED = 12,
    HARDWARE_CONFIG_DIGEST = 17
} hardware_config_result_t;

hardware_config_result_t hardware_config_verify(const unsigned char *data, unsigned size,
                                                 hardware_config_profile_t *config);

#endif
