param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('bootstrap', 'dependencies', 'build', 'package')]
    [string]$Phase
)

$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true') {
    throw 'This script only runs on GitHub Actions.'
}

$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$buildRoot = Split-Path $sourceRoot -Parent
$thirdPartyRoot = Join-Path $buildRoot 'ThirdParty'
$librariesRoot = Join-Path $buildRoot 'Libraries/win64'

function Get-DependencyCacheFingerprint {
    $entries = foreach ($cacheRoot in @($librariesRoot, $thirdPartyRoot)) {
        $keysPath = Join-Path $cacheRoot 'cache_keys'
        if (Test-Path -LiteralPath $keysPath) {
            Get-ChildItem -LiteralPath $keysPath -File | Sort-Object Name | ForEach-Object {
                "$cacheRoot/$($_.Name):$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)"
            }
        }
    }
    return ($entries -join '|')
}

function Invoke-NativeBuild([string[]]$Commands) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vsRoot = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsRoot) {
        throw 'Visual Studio 2022 with the C++ toolchain was not found.'
    }
    $vcvars = Join-Path $vsRoot 'VC/Auxiliary/Build/vcvars64.bat'
    $batchPath = Join-Path $env:RUNNER_TEMP "visugram-$Phase.cmd"
    $batchLines = @('@echo off', "call `"$vcvars`" -vcvars_ver=14.44 10.0.26100.0", 'if errorlevel 1 exit /b 1', "cd /d `"$sourceRoot\Telegram`"")
    foreach ($command in $Commands) {
        $batchLines += $command
        $batchLines += 'if errorlevel 1 exit /b 1'
    }
    $batchLines += 'exit /b 0'
    [IO.File]::WriteAllText($batchPath, ($batchLines -join "`r`n"), [Text.UTF8Encoding]::new($false))
    & cmd.exe /d /c $batchPath
    if ($LASTEXITCODE -ne 0) {
        throw "The $Phase phase failed with exit code $LASTEXITCODE."
    }
}

switch ($Phase) {
    'bootstrap' {
        [IO.File]::WriteAllText((Join-Path $env:RUNNER_TEMP 'visugram-cache-before.txt'), (Get-DependencyCacheFingerprint), [Text.UTF8Encoding]::new($false))
        $stages = @(
            @{ Name = 'patches'; Key = (Join-Path $librariesRoot 'cache_keys/patches') },
            @{ Name = 'msys64'; Key = (Join-Path $thirdPartyRoot 'cache_keys/msys64') },
            @{ Name = 'python'; Key = (Join-Path $thirdPartyRoot 'cache_keys/python') },
            @{ Name = 'NuGet'; Key = (Join-Path $thirdPartyRoot 'cache_keys/NuGet') },
            @{ Name = 'jom'; Key = (Join-Path $thirdPartyRoot 'cache_keys/jom') },
            @{ Name = 'gyp'; Key = (Join-Path $thirdPartyRoot 'cache_keys/gyp') }
        )
        foreach ($stage in $stages) {
            if (-not (Test-Path -LiteralPath $stage.Key)) {
                Invoke-NativeBuild @("call build\prepare\win.bat silent skip-release $($stage.Name)")
            }
        }
        $archivePath = Join-Path $env:RUNNER_TEMP 'nasm-3.01.zip'
        $extractPath = Join-Path $env:RUNNER_TEMP 'visugram-nasm-3.01'
        Invoke-WebRequest 'https://www.nasm.us/pub/nasm/releasebuilds/3.01/win64/nasm-3.01-win64.zip' -OutFile $archivePath
        Expand-Archive -LiteralPath $archivePath -DestinationPath $extractPath -Force
        foreach ($toolName in @('nasm.exe', 'ndisasm.exe')) {
            $toolPath = Get-ChildItem -LiteralPath $extractPath -Filter $toolName -Recurse -File | Select-Object -First 1
            if (-not $toolPath) {
                throw "$toolName was not found in the NASM archive."
            }
            Copy-Item -LiteralPath $toolPath.FullName -Destination (Join-Path $thirdPartyRoot "msys64/mingw64/bin/$toolName") -Force
        }
    }
    'dependencies' {
        Invoke-NativeBuild @('call build\prepare\win.bat silent skip-release')
        $beforePath = Join-Path $env:RUNNER_TEMP 'visugram-cache-before.txt'
        $before = [IO.File]::ReadAllText($beforePath)
        $cacheChanged = $before -cne (Get-DependencyCacheFingerprint)
        "cache_changed=$($cacheChanged.ToString().ToLowerInvariant())" | Out-File -LiteralPath $env:GITHUB_OUTPUT -Append -Encoding utf8
    }
    'build' {
        $apiId = if ($env:VISUGRAM_API_ID) { $env:VISUGRAM_API_ID } else { '2040' }
        $apiHash = if ($env:VISUGRAM_API_HASH) { $env:VISUGRAM_API_HASH } else { 'b18441a1ff607e10a989891a5462e627' }
        if ($apiId -notmatch '^\d+$' -or $apiHash -notmatch '^[a-fA-F0-9]{32}$') {
            throw 'The Telegram API credentials have an invalid format.'
        }
        Invoke-NativeBuild @(
            "call configure.bat `"-GNinja Multi-Config`" debug -DCMAKE_CONFIGURATION_TYPES=Debug -DTDESKTOP_API_ID=$apiId -DTDESKTOP_API_HASH=$apiHash -DDESKTOP_APP_DISABLE_AUTOUPDATE=ON -DDESKTOP_APP_DISABLE_CRASH_REPORTS=ON -DDESKTOP_APP_ENABLE_LTO=OFF -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded",
            'cmake --build ..\out --config Debug --target Telegram --parallel 4 -- -k 0'
        )
    }
    'package' {
        $binaryRoot = Join-Path $sourceRoot 'out/Debug'
        $binaryPath = Join-Path $binaryRoot 'VisuGram.exe'
        if (-not (Test-Path -LiteralPath $binaryPath)) {
            throw "The compiled executable was not found: $binaryPath"
        }
        $packageRoot = Join-Path $env:RUNNER_TEMP "VisuGram-package-$env:GITHUB_RUN_ID-$env:GITHUB_RUN_ATTEMPT"
        $distRoot = Join-Path $sourceRoot 'dist'
        New-Item -ItemType Directory -Path $packageRoot, $distRoot -Force | Out-Null
        Copy-Item -LiteralPath $binaryPath -Destination $packageRoot
        Get-ChildItem -LiteralPath $binaryRoot -Filter '*.dll' -File | Copy-Item -Destination $packageRoot
        foreach ($licenseName in @('LICENSE', 'LEGAL')) {
            Copy-Item -LiteralPath (Join-Path $sourceRoot $licenseName) -Destination $packageRoot
        }
        Copy-Item -LiteralPath (Join-Path $sourceRoot 'docs/portable-readme.txt') -Destination (Join-Path $packageRoot 'README.txt')
        $portableRoot = Join-Path $packageRoot 'TelegramForcePortable'
        New-Item -ItemType Directory -Path $portableRoot | Out-Null
        [IO.File]::WriteAllText((Join-Path $portableRoot 'portable.txt'), 'VisuGram portable data directory.', [Text.UTF8Encoding]::new($false))
        $zipName = "VisuGram-windows-x64-preview-$env:GITHUB_RUN_NUMBER.zip"
        $zipPath = Join-Path $distRoot $zipName
        Compress-Archive -Path (Join-Path $packageRoot '*') -DestinationPath $zipPath -Force -CompressionLevel Optimal
        $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
        [IO.File]::WriteAllText((Join-Path $distRoot 'SHA256SUMS.txt'), "$hash  $zipName`n", [Text.UTF8Encoding]::new($false))
        Write-Output "Packaged $zipName ($((Get-Item -LiteralPath $zipPath).Length) bytes)."
    }
}
