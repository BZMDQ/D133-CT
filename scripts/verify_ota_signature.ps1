param([string]$Package,[string]$Signature,[string]$PublicKey)
$ErrorActionPreference='Stop'
if([string]::IsNullOrWhiteSpace($Package)){$Package=Join-Path (Split-Path -Parent $PSScriptRoot) 'downloads\ota.cpio'}
if([string]::IsNullOrWhiteSpace($Signature)){$Signature=$Package+'.sig'}
if([string]::IsNullOrWhiteSpace($PublicKey)){$PublicKey=Join-Path (Split-Path -Parent $PSScriptRoot) 'source\application\rt-thread\d133_hmi\assets\ota_public.pem'}
if($PSVersionTable.PSVersion.Major -lt 7){throw 'RSA/SHA256 验签脚本需要 PowerShell 7 或更新版本。'}
$rsa=[System.Security.Cryptography.RSA]::Create()
try {
    $rsa.ImportFromPem([IO.File]::ReadAllText((Resolve-Path $PublicKey)))
    if(-not $rsa.VerifyData([IO.File]::ReadAllBytes((Resolve-Path $Package)),[IO.File]::ReadAllBytes((Resolve-Path $Signature)),[System.Security.Cryptography.HashAlgorithmName]::SHA256,[System.Security.Cryptography.RSASignaturePadding]::Pkcs1)){throw 'RSA/SHA256 signature is invalid'}
} finally {$rsa.Dispose()}
Write-Output 'RSA-2048/SHA256 signature: PASS'
