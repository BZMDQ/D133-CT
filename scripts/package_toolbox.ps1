param([string]$Python='D:\Users\ws2608007\Desktop\CSV0\STM32\tmp\winpython_py38_x86\venv\Scripts\python.exe',[string]$BuildBase='D:\Users\ws2608007\Desktop\CSV0\STM32\tmp')
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
$source=Join-Path $project 'toolbox\Source'
$guiSource=Get-Content -LiteralPath (Join-Path $source 'local_tcp_gui.py') -Raw -Encoding UTF8
$versionMatch=[regex]::Match($guiSource, "(?m)^APP_VERSION\s*=\s*'([^']+)'\s*$")
if(!$versionMatch.Success){throw '无法读取工具箱版本'}
$version=$versionMatch.Groups[1].Value
$versionToken=$version.Replace('.','')
if(!(Test-Path -LiteralPath $Python -PathType Leaf)){throw "需安装Python 3.8 32-bit及源码包依赖：$Python"}
$buildBase=$BuildBase
$work=Join-Path $buildBase "D133_HMI_toolbox${versionToken}_build"
if(Test-Path -LiteralPath $work){
    $resolvedWork=(Resolve-Path -LiteralPath $work).Path
    $resolvedBuild=(Resolve-Path -LiteralPath $buildBase).Path
    throw "Build folder already exists; preserve it and choose another BuildBase: $resolvedWork"
}
New-Item -ItemType Directory -Force $work | Out-Null
Set-Content (Join-Path $work '.d133_codex_build') 'D133CBS toolbox release build' -Encoding ASCII
$buildSource=Join-Path $work 'Source';$dist=Join-Path $work 'dist';$build=Join-Path $work 'build';New-Item -ItemType Directory -Force $buildSource,$dist,$build|Out-Null
Copy-Item (Join-Path $source '*') $buildSource -Recurse -Force
$output=Join-Path $dist "ResearchToolbox_$version.exe"
$previousPython=$env:TOOLBOX_PYTHON
try {
    $env:TOOLBOX_PYTHON=$Python
    & (Join-Path $buildSource 'build.ps1') -OneDir -OutputPath $output -WorkPath $build
    if($LASTEXITCODE -ne 0){throw "PyInstaller exited with code $LASTEXITCODE"}
} finally {$env:TOOLBOX_PYTHON=$previousPython}
if($LASTEXITCODE -ne 0){throw "Toolbox packaging failed: $LASTEXITCODE"}
$bundle=Join-Path $dist "ResearchToolbox_$version"
$destination=Join-Path $project "downloads\研发工具箱_$version"
if(Test-Path $destination){throw "Refusing to replace existing release: $destination"}
New-Item -ItemType Directory -Force (Split-Path $destination)|Out-Null
Copy-Item $bundle $destination -Recurse -Force
$installed='C:\Users\ws2608007\AppData\Local\Programs\研发工具箱'
foreach($folder in @('STM32CubeProgrammer','STLink_Driver','Licenses')){
    $from=Join-Path $installed $folder;$to=Join-Path $destination $folder
    if((Test-Path -LiteralPath $from -PathType Container) -and !(Test-Path $to)){Copy-Item $from $to -Recurse -Force}
}
Get-ChildItem $destination -Filter '*.exe' | Select-Object FullName,Length





