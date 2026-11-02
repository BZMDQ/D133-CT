# 本地 OTA 签名密钥

私钥不纳入 Git 仓库。开发机可将 RSA 私钥放在本目录的 `development_private.pem`，或在签名命令中使用 `-PrivateKey` 指定文件路径。

量产版本应使用受控密钥，不要把私钥复制到 `source/`、`downloads/` 或固件镜像中。
