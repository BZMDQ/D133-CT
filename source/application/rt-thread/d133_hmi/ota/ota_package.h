#ifndef HMI_OTA_PACKAGE_H
#define HMI_OTA_PACKAGE_H
#include <rtconfig.h>
#include <stdint.h>
/* Luban-Lite 将 Kconfig 字符串导出为 HMI_FW_VERSION 宏，统一用于界面和 OTA 版本比较。 */
#ifdef HMI_FW_VERSION
#define HMI_VERSION HMI_FW_VERSION
#else
#define HMI_VERSION "1.0.0"
#endif
#define HMI_PRODUCT "D133CBS"
#define HMI_HARDWARE "D133CBV-QFN88-V1-2"
/* 严格预检：CPIO CRC、完整结尾、文件白名单、槽位容量和版本均在写 Flash 前验证。 */
int ota_package_check(const char *path,char version[32]);
int ota_version_newer(const char *version);
int ota_package_sha256(const char *path,unsigned char digest[32]);
#endif


