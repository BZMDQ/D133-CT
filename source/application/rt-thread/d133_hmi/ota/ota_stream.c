#include "ota_stream.h"
#include "ota_manager.h"
#include "ota_signature.h"
#include "ota_package.h"
#include "boot_manager.h"
#include "system_manager.h"
#include <ota.h>
#include <absystem_os.h>
#include <aic_crc32.h>
#include <rtthread.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/* 校验边界与白名单，不替代官方 CPIO 解包和 Flash 引擎。 */
static struct {
    unsigned char header[144],meta[512],hash[32];
    char version[32];
    uint32_t total,checked,size,remaining,sum,expected;
    unsigned have,need,seen,member,padding,phase;
    int initialized,metadata_ok,trailer;
} stream;
static int hex_field(const unsigned char *p,uint32_t *out)
{
    uint32_t value=0;for(unsigned i=0;i<8;i++){
        unsigned c=p[i],digit;if(c>='0'&&c<='9')digit=c-'0';
        else if(c>='a'&&c<='f')digit=c-'a'+10;
        else if(c>='A'&&c<='F')digit=c-'A'+10;else return -1;
        value=(value<<4)|digit;
    }*out=value;return 0;
}
static int validate_bytes(const unsigned char *data,unsigned length)
{
    if(length>stream.total-stream.checked)return -1;
    while(length){
        if(stream.phase==3){if(*data)return -1;data++;length--;stream.checked++;continue;}
        if(stream.phase==2){unsigned n=length<stream.padding?length:stream.padding;
            data+=n;length-=n;stream.checked+=n;stream.padding-=n;
            if(!stream.padding){stream.phase=0;stream.have=0;stream.need=110;}continue;}
        if(stream.phase==1){unsigned n=length<stream.remaining?length:stream.remaining;
            for(unsigned i=0;i<n;i++)stream.sum+=data[i];
            if(stream.member==1)memcpy(stream.meta+stream.size-stream.remaining,data,n);
            data+=n;length-=n;stream.checked+=n;stream.remaining-=n;
            if(stream.remaining)continue;
            if(stream.sum!=stream.expected)return -1;
            if(stream.member==1){char version[32];uint32_t expected_crc;
                memcpy(&expected_crc,stream.meta,4);
                if(crc32(0,stream.meta+4,508)!=expected_crc || ota_metadata_check(stream.meta,version,stream.total) || strcmp(version,stream.version))return -1;
                stream.metadata_ok=1;
            }
            stream.padding=(4-(stream.size&3))&3;
            if(stream.padding)stream.phase=2;
            else{stream.phase=0;stream.have=0;stream.need=110;}continue;
        }
        unsigned n=length<stream.need-stream.have?length:stream.need-stream.have;
        memcpy(stream.header+stream.have,data,n);stream.have+=n;data+=n;length-=n;stream.checked+=n;
        if(stream.have!=stream.need)continue;
        uint32_t names,mode;
        if(stream.need==110){
            if(memcmp(stream.header,"070702",6)||hex_field(stream.header+94,&names)||!names||names>32)return -1;
            stream.need=(110+names+3)&~3U;continue;
        }
        if(hex_field(stream.header+94,&names)||hex_field(stream.header+54,&stream.size)||hex_field(stream.header+102,&stream.expected)||hex_field(stream.header+14,&mode))return -1;
        char *name=(char*)stream.header+110;
        if(name[names-1]||memchr(name,0,names-1))return -1;
        if(!strcmp(name,"TRAILER!!!")){
            if(stream.size||stream.expected||stream.seen!=3||!stream.metadata_ok)return -1;
            stream.trailer=1;stream.phase=3;continue;
        }
        if((mode&0170000)!=0100000)return -1;
        stream.member=0;
        if(!strcmp(name,"ota_info.bin")&&stream.size==512&&!stream.seen)stream.member=1;
        else if(!strcmp(name,"d13x_os.itb")&&stream.size&&stream.size<=5248U*1024U)stream.member=2;
        if(!stream.member||(stream.seen&stream.member)||(!stream.seen&&stream.member!=1))return -1;
        stream.seen|=stream.member;stream.remaining=stream.size;stream.sum=0;stream.phase=1;
    }return 0;
}
void ota_stream_abort(int error)
{
    if(stream.initialized){ota_deinit();stream.initialized=0;}
    ota_manager_stream_state(OTA_FAILED,0,error);ota_manager_phase(0);
}
int ota_stream_begin(uint32_t total,const unsigned char hash[32],const char *version,const unsigned char *signature,unsigned signature_size)
{
    boot_status_t boot;boot_manager_get(&boot);
    /* 官方引擎按ENV选择非活动OS槽；资源文件系统固定使用A区。 */
    if(!boot.confirmed||boot.resource!='A'||(boot.current!='A'&&boot.current!='B'))return -3;
    if(ota_manager_receive_begin(version))return -3;
    memset(&stream,0,sizeof(stream));stream.total=total;stream.need=110;
    memcpy(stream.hash,hash,32);rt_snprintf(stream.version,sizeof(stream.version),"%s",version);
    ota_manager_phase(1);
    if(ota_signature_verify_bytes(hash,signature,signature_size)){ota_stream_abort(-24);return -24;}
    if(ota_manager_stream_confirm()){ota_stream_abort(-14);return -14;}
    extern bool os_status,rodatafs_status,datafs_status;
    os_status=rodatafs_status=datafs_status=0;
    if(ota_init()){ota_stream_abort(-25);return -25;}
    stream.initialized=1;ota_manager_phase(3);ota_manager_stream_state(OTA_RECEIVE,0,0);
    return 0;
}
int ota_stream_feed(const unsigned char *data,unsigned length)
{
    if(!stream.initialized||!length)return -1;
    /* 首块必须覆盖元数据，以保证任何镜像写入前已验证映射白名单。 */
    if((!stream.checked&&length<636)||validate_bytes(data,length)||!stream.metadata_ok)return -1;
    for(unsigned offset=0;offset<length;){unsigned n=length-offset;if(n>2048)n=2048;
        if(ota_shard_download_fun((char*)data+offset,n)<0)return -1;
        offset+=n;rt_thread_mdelay(1);
    }return 0;
}
int ota_stream_finish(const unsigned char actual_hash[32])
{
    extern bool os_status;
    ota_manager_phase(4);ota_manager_stream_state(OTA_CHECK,100,0);
    if(!stream.initialized||stream.checked!=stream.total||!stream.trailer||memcmp(actual_hash,stream.hash,32)||!os_status){ota_stream_abort(-23);return -23;}
    ota_deinit();stream.initialized=0;
    ota_manager_phase(5);char record[96];
    rt_snprintf(record,sizeof(record),"TCP direct verified version=%s",stream.version);
    if(system_manager_record_upgrade(record))system_manager_log("升级校验通过，结果记录写入失败");
    /* 完整包摘要、CPIO校验及官方成员完成标志通过后才提交冗余ENV。 */
    if(aic_upgrade_end()){ota_stream_abort(-27);return -27;}
    boot_manager_refresh();ota_manager_stream_state(OTA_READY,100,0);ota_manager_phase(0);return 0;
}
