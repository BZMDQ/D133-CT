param([ValidateSet('OS','Bootloader','OtaCandidate','OtaProfile')][string]$Target='OS',[int]$Jobs=8,[string]$LogName='final_build.log',[string]$SdkRoot=(Join-Path $PSScriptRoot '..\..\..\D133CES\luban-lite-master'),[string]$FirmwareVersion='', [string]$ToolboxKeyFile='')
$ErrorActionPreference='Stop'
$project=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sdk=(Resolve-Path $SdkRoot).Path
$overlay=(Resolve-Path (Join-Path $project 'source')).Path
$python=Join-Path $sdk 'tools\env\tools\Python38\python3.exe'
if(!(Test-Path -LiteralPath $python)){throw "SDK Python not found: $python"}
# 每次构建前按原始 BMP 生成横向正读的 LVGL RGB565 资源。
$logoBmp=Join-Path $overlay 'application\rt-thread\d133_hmi\ui\assets\wslogo.bmp'
$logoC=Join-Path $overlay 'application\rt-thread\d133_hmi\ui\hmi_logo.c'
& $python (Join-Path $project 'scripts\convert_hmi_logo.py') --input $logoBmp --output $logoC
if($LASTEXITCODE){throw '生成 LVGL Logo 资源失败'}
# 按文件同步到 SDK 实际路径，避免目录套叠；不覆盖构建生成的 bootloader.bin。
foreach($file in Get-ChildItem -LiteralPath $overlay -File -Recurse){
    $relative=$file.FullName.Substring($overlay.Length+1)
    if($relative.EndsWith('pack\bootloader.bin') -or $relative.EndsWith('pack\.image_cfg.json.tmp')){continue}
    $destination=Join-Path $sdk $relative
    New-Item -ItemType Directory -Force (Split-Path $destination)|Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
}
function Set-D133ImageVersion([string]$Path,[string]$Version){
    if(!(Test-Path -LiteralPath $Path)){return}
    $text=Get-Content -LiteralPath $Path -Raw
    $match=[regex]::Match($text,'("version"\s*:\s*")[^"]+(")')
    if(!$match.Success){throw "Image config version field not found: $Path"}
    $value=$match.Groups[1].Value+$Version+$match.Groups[2].Value
    $text=$text.Remove($match.Index,$match.Length).Insert($match.Index,$value)
    # Python 的配置解析器不接受 Windows PowerShell 5.1 写出的 UTF-8 BOM。
    [IO.File]::WriteAllText($Path,$text,[Text.UTF8Encoding]::new($false))
}
$previousPath=$env:PATH;$previousPythonPath=$env:PYTHONPATH
$privateKeyHeader=Join-Path $sdk 'application\rt-thread\d133_hmi\network\hmi_toolbox_key_private.h'
$privateKeyHeaderCreated=$false
try {
    $env:PATH=(Split-Path $python)+';'+(Join-Path $sdk 'toolchain\bin')+';'+(Join-Path $sdk 'tools\env\tools\bin')+';'+$previousPath
    $env:PYTHONPATH=Join-Path $sdk 'tools\env\tools\Python27\Lib\site-packages\scons'
    if($FirmwareVersion -and $FirmwareVersion -notmatch '^\d+\.\d+\.\d+$'){throw 'FirmwareVersion must use semantic x.y.z format.'}
    if($FirmwareVersion -and $Target -eq 'Bootloader'){throw 'FirmwareVersion applies to OS and OTA targets, not Bootloader.'}
    if(Test-Path -LiteralPath $privateKeyHeader){
        $oldHeader=Get-Content -LiteralPath $privateKeyHeader -Raw -Encoding UTF8
        if($oldHeader.Contains('HMI_TOOLBOX_KEY_HEADER_GENERATED')){
            Remove-Item -LiteralPath $privateKeyHeader -Force
        }else{
            throw 'Private toolbox key header already exists in SDK; refusing to overwrite it.'
        }
    }
    if($ToolboxKeyFile){
        $keyPath=(Resolve-Path -LiteralPath $ToolboxKeyFile).Path
        $keyText=[IO.File]::ReadAllText($keyPath,[Text.Encoding]::ASCII).Trim()
        if($keyText -cnotmatch '^[0-9a-fA-F]{32}$'){throw 'Toolbox Key file must contain exactly 32 hexadecimal characters.'}
        $headerText="/* HMI_TOOLBOX_KEY_HEADER_GENERATED: local-only build input */`r`n#ifndef HMI_TOOLBOX_KEY_PRIVATE_H`r`n#define HMI_TOOLBOX_KEY_PRIVATE_H`r`n#define HMI_TOOLBOX_KEY_HEX `"$($keyText.ToLowerInvariant())`"`r`n#endif`r`n"
        [IO.File]::WriteAllText($privateKeyHeader,$headerText,[Text.UTF8Encoding]::new($false))
        $privateKeyHeaderCreated=$true
        $keyText=$null
        $headerText=$null
    }
$isOtaTarget=$Target -eq 'OtaCandidate' -or $Target -eq 'OtaProfile'
$def=if($Target -eq 'Bootloader'){'d13x_d133cbv-hmi-nor_baremetal_bootloader_defconfig'}elseif($Target -eq 'OtaProfile'){'d13x_d133cbv-hmi-nor_rt-thread_d133_hmi_ota_profile_defconfig'}elseif($Target -eq 'OtaCandidate'){'d13x_d133cbv-hmi-nor_rt-thread_d133_hmi_ota_defconfig'}else{'d13x_d133cbv-hmi-nor_rt-thread_d133_hmi_defconfig'}
    $sourceImageCfg=Join-Path $overlay 'target\d13x\d133cbv-hmi-nor\pack\image_cfg.json'
    $versionConfig=Join-Path $sdk (Join-Path 'target\configs' $def)
    if($FirmwareVersion){
        $configText=Get-Content -LiteralPath $versionConfig -Raw -Encoding UTF8
        # 只替换版本文本本身，保留 CRLF 行结束，避免无 BOM 配置被覆盖成混合行尾。
        $versionPattern='(?m)^CONFIG_HMI_FW_VERSION="[0-9]+\.[0-9]+\.[0-9]+"'
        if($configText -notmatch $versionPattern){throw 'Selected defconfig has no HMI firmware version.'}
        $configText=[regex]::Replace($configText,$versionPattern,'CONFIG_HMI_FW_VERSION="'+$FirmwareVersion+'"')
        [IO.File]::WriteAllText($versionConfig,$configText,[Text.UTF8Encoding]::new($false))
        $version=$FirmwareVersion
    }
    if($isOtaTarget){
        $versionLine=Select-String -Path $versionConfig -Pattern '^CONFIG_HMI_FW_VERSION="([0-9]+\.[0-9]+\.[0-9]+)"$' | Select-Object -First 1
        if(!$versionLine){throw 'OTA candidate defconfig has no semantic firmware version'}
        if(!$FirmwareVersion){$version=$versionLine.Matches[0].Groups[1].Value}
    }elseif(!$FirmwareVersion){
        $imageText=Get-Content $sourceImageCfg -Raw
        $versionMatch=[regex]::Match($imageText,'("version"\s*:\s*")[^"]+(")')
        if(!$versionMatch.Success){throw 'Factory image_cfg has no product version'}
        $version=$versionMatch.Groups[0].Value -replace '^.*"([^\"]+)"$','$1'
    }
    Set-D133ImageVersion (Join-Path $sdk 'target\d13x\d133cbv-hmi-nor\pack\image_cfg.json') $version
    $versionAsset=Join-Path $sdk 'application\rt-thread\d133_hmi\assets\version.txt'
    if(Test-Path -LiteralPath $versionAsset){[IO.File]::WriteAllText($versionAsset,"D133-控制终端 $version`r`n",[Text.UTF8Encoding]::new($false))}
    # 隐藏的 SDK 临时镜像配置也需采用工程分区表，避免镜像打包静默沿用旧槽位大小。
    $overlayImageCfgTmp=Join-Path $overlay 'target\d13x\d133cbv-hmi-nor\pack\.image_cfg.json.tmp'
    if(Test-Path -LiteralPath $overlayImageCfgTmp){
        Copy-Item -LiteralPath $overlayImageCfgTmp -Destination (Join-Path $sdk 'target\d13x\d133cbv-hmi-nor\pack\.image_cfg.json.tmp') -Force
    }
    Set-D133ImageVersion (Join-Path $sdk 'target\d13x\d133cbv-hmi-nor\pack\.image_cfg.json.tmp') $version
    Push-Location $sdk
    try {
        & $python -c 'import SCons.Script; SCons.Script.main()' "--apply-def=$def"
        if($LASTEXITCODE){throw '加载 defconfig 失败'}
        if($isOtaTarget){
            # 避免候选配置的自标识指回工厂 profile，导致 SDK 静默复用旧镜像。
            $appliedProfile=Select-String -Path (Join-Path $sdk '.config') -Pattern '^CONFIG_PRJ_DEFCONFIG_FILENAME="([^"]+)"$' | Select-Object -First 1
            $appliedVersion=Select-String -Path (Join-Path $sdk '.config') -Pattern '^CONFIG_HMI_FW_VERSION="([0-9]+\.[0-9]+\.[0-9]+)"$' | Select-Object -First 1
            if(!$appliedProfile -or $appliedProfile.Matches[0].Groups[1].Value -ne $def){throw 'OTA 候选 profile 自标识与所选配置不一致'}
            if(!$appliedVersion -or $appliedVersion.Matches[0].Groups[1].Value -ne $version){throw 'OTA 固件版本未写入当前 SDK 配置'}
        }
        if($Target -eq 'OtaProfile'){
            $initProfile=Select-String -Path (Join-Path $sdk '.config') -Pattern '^CONFIG_RT_DEBUG_INIT_CONFIG=y$' | Select-Object -First 1
            if(!$initProfile){throw 'RT 初始化诊断 profile 未启用逐项耗时日志'}
        }
        $buildLog=Join-Path $project "docs\$LogName"
        $previousErrorAction=$ErrorActionPreference
        try {
            # Windows PowerShell 5.1 会把 SCons 的标准错误进度流提升为异常；在此仅收集输出，再按退出码判断。
            $ErrorActionPreference='Continue'
            & $python -c 'import SCons.Script; SCons.Script.main()' "-j$Jobs" 2>&1 | Out-File -LiteralPath $buildLog -Encoding utf8
            $buildExit=$LASTEXITCODE
        } finally {
            $ErrorActionPreference=$previousErrorAction
        }
        if($buildExit){throw "编译失败，见 docs\$LogName"}
    } finally {Pop-Location}
} finally {
    if($privateKeyHeaderCreated -and (Test-Path -LiteralPath $privateKeyHeader)){Remove-Item -LiteralPath $privateKeyHeader -Force}
    $env:PATH=$previousPath;$env:PYTHONPATH=$previousPythonPath
}



