#include "ota_sd.h"
#include "hmi_serial_log.h"
#include "ota_manager.h"
#include <rtthread.h>
/* 用户主动调用命令后，由 OTA 工作线程校验包和签名，命令线程不直接写 Flash。 */
int ota_sd_start(void){return ota_manager_request("/sdcard/UPDATE/ota.cpio",0,0,0);}
static void sd_ota_command(void){hmi_serial_log("[sd-ota] request=%d\n",ota_sd_start());}
MSH_CMD_EXPORT_ALIAS(sd_ota_command,hmi_sd_ota,Install signed SD CPIO to inactive slots);

