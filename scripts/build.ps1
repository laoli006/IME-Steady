[CmdletBinding()]
param([string]$VcVars64 = '')

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

function Invoke-Native([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

try {
    if (!$VcVars64) { $VcVars64 = $env:VCVARS64 }
    if (!$VcVars64) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $found = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC\Auxiliary\Build\vcvars64.bat'
            if ($found) { $VcVars64 = @($found)[0] }
        }
    }
    if ($VcVars64) {
        Write-Host 'Loading MSVC x64 environment...'
        $environment = & $env:ComSpec /d /c "call `"$VcVars64`" >nul && set"
        if ($LASTEXITCODE -ne 0) { throw 'vcvars64.bat failed.' }
        foreach ($line in $environment) {
            if ($line -match '^([^=]+)=(.*)$') {
                [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
            }
        }
    }
    if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64') {
        throw 'Install the Visual Studio C++ x64 tools and Windows SDK, or pass -VcVars64 <path>.'
    }
    $compiler = (Get-Command cl.exe -ErrorAction Stop).Source
    $resourceCompiler = (Get-Command rc.exe -ErrorAction Stop).Source
    foreach ($name in @('CL', '_CL_', 'LINK', '_LINK_')) {
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }

    foreach ($directory in @('build\payload', 'build\generated', 'dist')) {
        New-Item -ItemType Directory -Force -Path (Join-Path $root $directory) | Out-Null
    }
    $common = @('/nologo', '/O2', '/MT', '/EHsc', '/DUNICODE', '/D_UNICODE')
    Push-Location (Join-Path $root 'build\payload')
    try {
        Write-Host 'Building runtime files...'
        Invoke-Native $compiler ($common + @('/std:c++17', '..\..\src\indicator.cpp',
            '/Foindicator.obj', '/FeIMESteadyTray.exe', '/link', '/SUBSYSTEM:WINDOWS', '/INCREMENTAL:NO'))
        Invoke-Native $compiler ($common + @('/std:c++20', '..\..\src\cheapInject\main.cpp',
            '/FocheapInject.obj', '/FecheapInject.exe', '/link', '/INCREMENTAL:NO'))
        Invoke-Native $compiler ($common + @('/std:c++17', '/LD', '..\..\src\langhook.cpp',
            '/Folanghook.obj', '/Felanghook64.dll', 'user32.lib', '/link', '/INCREMENTAL:NO'))
        Invoke-Native $compiler ($common + @('/std:c++17', '/LD', '/I..\..\third_party\minhook\include',
            '..\..\src\main.cpp', '..\..\third_party\minhook\src\buffer.c',
            '..\..\third_party\minhook\src\hook.c', '..\..\third_party\minhook\src\trampoline.c',
            '..\..\third_party\minhook\src\hde\hde64.c', '..\..\third_party\minhook\src\hde\hde32.c',
            '/Femymspinyin.dll', '/link', '/INCREMENTAL:NO'))
    } finally { Pop-Location }

    # Each build extracts into its own folder; repeated runs reuse that folder.
    $generated = Join-Path $root 'build\generated'
    $buildId = [Guid]::NewGuid().ToString('N')
    [IO.File]::WriteAllText((Join-Path $generated 'build_id.h'), "#pragma once`n#define BUILD_ID L`"$buildId`"`n", [Text.Encoding]::ASCII)
    $rcLines = @('#include "resource.h"', '1 24 "../../launcher/launcher.manifest"')
    $files = [ordered]@{
        ID_PAYLOAD_INJECTOR = '../payload/cheapInject.exe'
        ID_PAYLOAD_OBSERVER = '../payload/mymspinyin.dll'
        ID_PAYLOAD_INDICATOR = '../payload/IMESteadyTray.exe'
        ID_PAYLOAD_LANGHOOK = '../payload/langhook64.dll'
        ID_README = '../../README.md'
        ID_LICENSE = '../../LICENSE.txt'
        ID_THIRD_PARTY = '../../THIRD_PARTY_NOTICES.txt'
        ID_WINDOWS_INDICATOR_SCRIPT = '../../scripts/WindowsIMEIndicator.ps1'
        ID_WINDOWS_INDICATOR_MENU = '../../WindowsIMEIndicator.cmd'
    }
    foreach ($id in $files.Keys) { $rcLines += $id + ' RCDATA "' + $files[$id] + '"' }
    [IO.File]::WriteAllLines((Join-Path $generated 'payload.rc'), [string[]]$rcLines, [Text.Encoding]::Unicode)

    Push-Location $generated
    try {
        Write-Host 'Building single-file launcher...'
        Invoke-Native $resourceCompiler @('/nologo', '/I..\..\launcher', '/fopayload.res', 'payload.rc')
        Invoke-Native $compiler ($common + @('/W4', '/std:c++17', '/I.', '..\..\launcher\launcher.cpp',
            '/Fo..\launcher.obj', '/Fe..\..\dist\IMESteady.exe', 'payload.res',
            '/link', '/SUBSYSTEM:WINDOWS', '/INCREMENTAL:NO', '/MANIFEST:NO'))
    } finally { Pop-Location }
    Write-Host 'Ready: dist\IMESteady.exe'
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
