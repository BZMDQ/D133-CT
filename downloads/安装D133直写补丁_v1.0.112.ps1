param([string]$TargetDirectory='D:\Users\ws2608007\AppData\Local\Programs\研发工具箱')
$ErrorActionPreference='Stop'
# 手动安装：不控制正在运行的程序，不执行任何硬件升级。
if(Get-Process -ErrorAction SilentlyContinue | Where-Object ProcessName -Like 'ResearchToolbox*'){throw '请先手动退出研发工具箱。'}
$target=Join-Path $TargetDirectory 'd133_ota_client.pyc'
if(!(Test-Path -LiteralPath $target)){throw '未找到工具箱客户端。'}
if((Get-FileHash -Algorithm SHA256 $target).Hash -ne 'A1CE9B112270E640BD0F7B6F0E801F8BB78482F993FBC410920B37C867652B2C'){throw '模块版本不匹配，未修改。'}
$backup=Join-Path $TargetDirectory ('D133_TCP_Backup_'+(Get-Date -Format yyyyMMdd_HHmmss))
New-Item -ItemType Directory -Path $backup | Out-Null
Copy-Item -LiteralPath $target -Destination $backup
$source=Join-Path $TargetDirectory 'Source\d133_ota_client.py'
if(Test-Path -LiteralPath $source){Copy-Item -LiteralPath $source -Destination $backup}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'd133_ota_client.pyc') -Destination $target -Force
if(Test-Path -LiteralPath (Split-Path $source)){Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'd133_ota_client.py') -Destination $source -Force}
Write-Output "安装完成，备份：$backup。请手动打开工具箱。"
