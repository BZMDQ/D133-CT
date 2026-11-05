param(
    [Parameter(Mandatory=$true)][string]$InputFile,
    [Parameter(Mandatory=$true)][string]$OutputFile,
    [string]$FFmpeg='ffmpeg'
)
$ErrorActionPreference='Stop'
# 仅生成视频画面；保持纵横比并限制为 720P 和 60 帧/秒。
$inputPath=(Resolve-Path -LiteralPath $InputFile).Path
$outputPath=[IO.Path]::GetFullPath($OutputFile)
if ([IO.Path]::GetExtension($outputPath).ToLowerInvariant() -notin @('.mp4','.mov','.avi')) { throw '输出文件必须为 .mp4、.mov 或 .avi' }
if ($inputPath -eq $outputPath) { throw '输入和输出不能是同一个文件' }
if (Test-Path -LiteralPath $outputPath) { throw '输出文件已存在，请指定新的文件名' }
# 限制峰值码率以减轻 SD 卡持续读取压力；保留 720P 和 60 帧时间戳。
& $FFmpeg -nostdin -n -i $inputPath -vf 'scale=1280:720:force_original_aspect_ratio=decrease:force_divisible_by=2,fps=60' -an -c:v mjpeg -b:v 10M -maxrate 12M -bufsize 20M -pix_fmt yuvj420p $outputPath
if ($LASTEXITCODE -ne 0) { throw "视频转换失败，FFmpeg 退出码：$LASTEXITCODE" }
Write-Host "已生成：$outputPath"
