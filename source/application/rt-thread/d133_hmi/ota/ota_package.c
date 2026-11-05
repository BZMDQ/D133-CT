#include "ota_package.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mbedtls/sha256.h>

static int hex8(const char *p,uint32_t *value)
{
    unsigned i;*value=0;
    for(i=0;i<8;i++){
        unsigned c=(unsigned char)p[i],digit;
        if(c>='0'&&c<='9')digit=c-'0';
        else if(c>='a'&&c<='f')digit=c-'a'+10;
        else if(c>='A'&&c<='F')digit=c-'A'+10;
        else return -1;
        *value=(*value<<4)|digit;
    }
    return 0;
}
static void trim(char *s)
{
    size_t n=strlen(s);while(n&&s[n-1]==' ')s[--n]=0;
    while(*s==' ')memmove(s,s+1,strlen(s));
}
/* ota_info.bin 的字段清单固定为官方 OS 与 RODATA 映射，拒绝额外目标。 */
static int metadata_check(const unsigned char meta[512],char version[32],uint32_t archive_size)
{
    static const char *expected[]={"[image]","$SIZE","$VERSION","[file]",
                                   "ota_info.bin:file;","d13x_os.itb:os;",
                                   "rodata.fatfs:rodata;"};
    char line[128],size_text[32],version_text[32];unsigned index=0,pos=4;size_t n;
    size_text[0]=version_text[0]=0;
    while(pos<512){
        unsigned start=pos;
        if(meta[pos]==0||meta[pos]==0xff)break;
        while(pos<512&&meta[pos])pos++;
        n=pos-start;if(pos>=512||!n||n>=sizeof(line))return -1;
        memcpy(line,meta+start,n);line[n]=0;trim(line);
        if(index>=sizeof(expected)/sizeof(expected[0]))return -1;
        if(index==1){
            const char *a="size = \"",*end;
            if(strncmp(line,a,strlen(a))||(end=strchr(line+strlen(a),'"'))==NULL||end[1]!=';'||end==line+strlen(a))return -1;
            n=(size_t)(end-(line+strlen(a)));if(n>=sizeof(size_text))return -1;
            memcpy(size_text,line+strlen(a),n);size_text[n]=0;
            for(n=0;size_text[n];n++)if(size_text[n]<'0'||size_text[n]>'9')return -1;
        }else if(index==2){
            const char *a="version = \"",*end;
            if(strncmp(line,a,strlen(a))||(end=strchr(line+strlen(a),'"'))==NULL||end[1]!=';')return -1;
            n=(size_t)(end-(line+strlen(a)));if(!n||n>=sizeof(version_text))return -1;
            memcpy(version_text,line+strlen(a),n);version_text[n]=0;
        }else if(strcmp(line,expected[index]))return -1;
        index++;pos++;
    }
    /* 仅允许 mkenvimage 的 0xFF 填充，禁止第二段隐藏配置。 */
    for(;pos<512;pos++)if(meta[pos]!=0&&meta[pos]!=0xff)return -1;
    if(index!=7||!size_text[0]||!version_text[0]||strtoul(size_text,NULL,10)!=archive_size)return -1;
    snprintf(version,32,"%s",version_text);return 0;
}
int ota_version_newer(const char *s)
{
    unsigned a,b,c,x,y,z;char tail;
    if(sscanf(s,"%u.%u.%u%c",&a,&b,&c,&tail)!=3||sscanf(HMI_VERSION,"%u.%u.%u",&x,&y,&z)!=3)return 0;
    return a>x||(a==x&&(b>y||(b==y&&c>z)));
}
int ota_package_sha256(const char *path,unsigned char hash[32])
{
    FILE *f=fopen(path,"rb");unsigned char b[2048];size_t n;int r=0;mbedtls_sha256_context ctx;
    if(!f)return -1;
    mbedtls_sha256_init(&ctx);r=mbedtls_sha256_starts_ret(&ctx,0);
    while(!r&&(n=fread(b,1,sizeof(b),f)))r=mbedtls_sha256_update_ret(&ctx,b,n);
    if(ferror(f))r=-1;
    if(!r)r=mbedtls_sha256_finish_ret(&ctx,hash);
    mbedtls_sha256_free(&ctx);fclose(f);return r;
}
int ota_package_check(const char *path,char version[32])
{
    FILE *f=fopen(path,"rb");char header[110],name[64],buffer[2048];unsigned char meta[512];
    uint32_t size,names,expected,sum,mode,remaining,archive_size;unsigned seen=0;int result=-1,tail;long file_size;
    if(!f)return -1;
    if(fseek(f,0,SEEK_END)||(file_size=ftell(f))<0||fseek(f,0,SEEK_SET)){fclose(f);return -1;}
    archive_size=(uint32_t)file_size;version[0]=0;
    while(fread(header,1,sizeof(header),f)==sizeof(header)){
        if(memcmp(header,"070702",6)||hex8(header+14,&mode)||hex8(header+54,&size)||hex8(header+94,&names)||hex8(header+102,&expected)||!names||names>sizeof(name))goto done;
        if(fread(name,1,names,f)!=names||name[names-1]||memchr(name,0,names-1))goto done;
        if(fseek(f,(4-((110+names)&3))&3,SEEK_CUR))goto done;
        if(!strcmp(name,"TRAILER!!!")){
            if(size||expected||seen!=7||!ota_version_newer(version))goto done;
            while((tail=fgetc(f))!=EOF)if(tail)goto done;
            if(ferror(f))goto done;
            result=0;goto done;
        }
        if((mode&0170000)!=0100000)goto done;
        unsigned flag=0;
        if(!strcmp(name,"ota_info.bin")&&size==512)flag=1;
        else if(!strcmp(name,"d13x_os.itb")&&size&&size<=3473408)flag=2;
        else if(!strcmp(name,"rodata.fatfs")&&size&&size<=3997696)flag=4;
        if(!flag||(seen&flag)||(!seen&&flag!=1))goto done;
        seen|=flag;sum=0;remaining=size;uint32_t offset=0;
        while(remaining){size_t n=remaining>sizeof(buffer)?sizeof(buffer):remaining;
            if(fread(buffer,1,n,f)!=n)goto done;
            for(size_t i=0;i<n;i++)sum+=(unsigned char)buffer[i];
            if(flag==1){memcpy(meta+offset,buffer,n);offset+=(uint32_t)n;}
            remaining-=(uint32_t)n;
        }
        if(sum!=expected)goto done;
        if(flag==1&&metadata_check(meta,version,archive_size))goto done;
        if(fseek(f,(4-(size&3))&3,SEEK_CUR))goto done;
    }
done:fclose(f);return result;
}


