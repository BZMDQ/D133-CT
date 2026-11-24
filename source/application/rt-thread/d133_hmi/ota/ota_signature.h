#ifndef HMI_OTA_SIGNATURE_H
#define HMI_OTA_SIGNATURE_H
/* RSA/SHA256 验证；公钥随只读资源部署，签名为 path + .sig。 */
int ota_signature_verify(const char *path,const unsigned char digest[32]);
int ota_signature_verify_bytes(const unsigned char digest[32],const unsigned char *signature,unsigned length);
#endif
