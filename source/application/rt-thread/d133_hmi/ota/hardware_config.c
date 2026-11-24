#include <rtconfig.h>
#include "hardware_config.h"

#if defined(HMI_USB_CDC_CONTROL) && defined(HMI_DISPLAY_PROFILE_SERIAL)
#include "ota_package.h"
#include "ota_signature.h"
#include "d133_wf101_profile.h"
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>

#define PAYLOAD_LIMIT 128U
#define SIGNATURE_SIZE 256U
#define BOARD_FIELDS HMI_PRODUCT "\n" HMI_HARDWARE "\n"

static const char headers[][sizeof("D133-HWCFG/1\n" BOARD_FIELDS)] = {
    "D133-HWCFG/1\n" BOARD_FIELDS,
    "D133-HWCFG/2\n" BOARD_FIELDS,
    "D133-HWCFG/3\n" BOARD_FIELDS,
};

/* V1/V2 验证旧签名；V3 是仅用于登记的可移植资料，以摘要检查传输完整性。 */
hardware_config_result_t hardware_config_verify(const unsigned char *data, unsigned size,
                                                 hardware_config_profile_t *config)
{
    unsigned char digest[32];
    const size_t header_size = sizeof(headers[0]) - 1U;
    const size_t slot_size = DISPLAY_PROFILE_SLOT_LENGTH - 1U;
    size_t code_size = slot_size, payload_size, trailer;
    display_profile_info_t profile;
    char expected[PAYLOAD_LIMIT + 1U];
    const unsigned char *slot_field;
    const char *display_ic;
    unsigned format;
    int written;

    if (size <= header_size + sizeof(digest) || size > PAYLOAD_LIMIT + SIGNATURE_SIZE)
        return HARDWARE_CONFIG_INVALID;
    for (format = 0; format < sizeof(headers) / sizeof(headers[0]); ++format)
        if (!memcmp(data, headers[format], header_size))
            break;
    if (format == sizeof(headers) / sizeof(headers[0]))
        return HARDWARE_CONFIG_INVALID;

    trailer = format == 2U ? sizeof(digest) : SIGNATURE_SIZE;
    if (size <= header_size + slot_size + trailer || size - trailer > PAYLOAD_LIMIT)
        return HARDWARE_CONFIG_INVALID;
    payload_size = size - trailer;
    slot_field = data + header_size;
    if (format != 0U) {
        const unsigned char *code_end = memchr(slot_field, '\n', payload_size - header_size);
        if (!code_end)
            return HARDWARE_CONFIG_INVALID;
        code_size = (size_t)(code_end - slot_field);
        if (!code_size || code_size >= DISPLAY_PROFILE_CODE_LENGTH)
            return HARDWARE_CONFIG_INVALID;
        slot_field = code_end + 1;
    }
    if ((size_t)(data + payload_size - slot_field) <= slot_size || slot_field[slot_size] != '\n')
        return HARDWARE_CONFIG_INVALID;

    if (mbedtls_sha256_ret(data, payload_size, digest, 0) != 0)
        return HARDWARE_CONFIG_DIGEST;
    if (format == 2U) {
        if (memcmp(digest, data + payload_size, sizeof(digest)))
            return HARDWARE_CONFIG_DIGEST;
    } else if (ota_signature_verify_bytes(digest, data + payload_size, SIGNATURE_SIZE) != 0) {
        return HARDWARE_CONFIG_SIGNATURE;
    }

    memcpy(config->code, data + header_size, code_size);
    config->code[code_size] = '\0';
    memcpy(config->slot, slot_field, slot_size);
    config->slot[slot_size] = '\0';
    if (display_profile_get_configured(config->slot, &profile) != DISPLAY_PROFILE_OK)
        return HARDWARE_CONFIG_UNSUPPORTED;

    config->custom = format == 1U ||
                     (format == 2U && strcmp(config->code, profile.code) && strcmp(config->code, config->slot));
    if (config->custom && !display_profile_custom_code(config->code))
        return HARDWARE_CONFIG_UNSUPPORTED;

    display_ic = display_profile_ic(config->slot);
    if (!strcmp(config->code, D133_WF101_CODE)) {
        /* 只接受明确标出无触摸的可移植配置，保持其他配对的校验不变。 */
        if (format != 2U || strcmp(config->slot, D133_WF101_SLOT))
            return HARDWARE_CONFIG_UNSUPPORTED;
        display_ic = D133_WF101_DISPLAY_IC;
        snprintf(profile.touch, sizeof(profile.touch), "%s", D133_WF101_TOUCH);
    }

    /* 代号之外的参数都与固件内实际启用的屏幕时序及触摸驱动逐字节核对。 */
    if (format != 0U)
        written = snprintf(expected, sizeof(expected), "%s%s\n%s\n%s\n%s\n%s\n%u,%u\n",
                           headers[format], config->code, config->slot, profile.panel,
                           display_ic, profile.touch,
                           (unsigned)profile.width, (unsigned)profile.height);
    else
        written = snprintf(expected, sizeof(expected), "%s%s\n%s\n%s\n%u,%u\n",
                           headers[format], config->slot, profile.panel, profile.touch,
                           (unsigned)profile.width, (unsigned)profile.height);
    if (written < 0 || (size_t)written != payload_size || (size_t)written >= sizeof(expected) ||
        memcmp(data, expected, payload_size) != 0)
        return HARDWARE_CONFIG_UNSUPPORTED;
    return HARDWARE_CONFIG_OK;
}
#endif
