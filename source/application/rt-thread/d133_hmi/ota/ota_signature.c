#include "ota_signature.h"
#include <stdio.h>
#include <string.h>
#include <mbedtls/pk.h>
int ota_signature_verify(const char *path,const unsigned char hash[32])
{
    char pem[2048]={0},name[280];unsigned char signature[512];size_t n,keylen;FILE *f;int result=-1;mbedtls_pk_context key;
    snprintf(name,sizeof(name),"%s.sig",path);f=fopen(name,"rb");if(!f)return -1;n=fread(signature,1,sizeof(signature),f);if(fgetc(f)!=EOF||ferror(f)){fclose(f);return -1;}fclose(f);if(!n)return -1;
    f=fopen("/rodata/hmi/ota_public.pem","rb");if(!f)return -2;keylen=fread(pem,1,sizeof(pem)-1,f);if(fgetc(f)!=EOF||ferror(f)){fclose(f);return -2;}fclose(f);
    mbedtls_pk_init(&key);result=mbedtls_pk_parse_public_key(&key,(unsigned char*)pem,keylen+1);
    if(!result&&!mbedtls_pk_can_do(&key,MBEDTLS_PK_RSA))result=-3;
    if(!result)result=mbedtls_pk_verify(&key,MBEDTLS_MD_SHA256,hash,32,signature,n);
    mbedtls_pk_free(&key);return result;
}
