param([Parameter(Mandatory=$true)][string]$Package,[string]$PrivateKey)
$ErrorActionPreference='Stop'
if([string]::IsNullOrWhiteSpace($PrivateKey)){$PrivateKey=Join-Path $PSScriptRoot '..\keys\development_private.pem'}
if($PSVersionTable.PSVersion.Major -lt 7){throw '签名脚本需要 PowerShell 7 或更新版本。'}
# 开发用离线签名。量产必须显式传入受控私钥，私钥绝不进入固件或工具箱。
$rsa=[System.Security.Cryptography.RSA]::Create()
try {
    $rsa.ImportFromPem([IO.File]::ReadAllText((Resolve-Path $PrivateKey)))
    $data=[IO.File]::ReadAllBytes((Resolve-Path $Package))
    $signature=$rsa.SignData($data,[System.Security.Cryptography.HashAlgorithmName]::SHA256,[System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
    [IO.File]::WriteAllBytes(([IO.Path]::GetFullPath($Package)+'.sig'),$signature)
} finally {$rsa.Dispose()}
