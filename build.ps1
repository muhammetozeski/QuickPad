#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Builds QuickPad.exe with MSVC, without the C runtime library.

.DESCRIPTION
    Imports the x64 build environment from vcvars64.bat, compiles src\*.c and links
    bin\QuickPad.exe. vcvars64.bat is taken from -VcVars when given, otherwise from the
    Visual Studio installation reported by vswhere.exe.

.PARAMETER VcVars
    Full path of vcvars64.bat, for Visual Studio installations vswhere.exe does not know about.

.PARAMETER Test
    Also builds the programs in tests\ and runs the unit tests.
#>
[CmdletBinding()]
param(
    [string]$VcVars,
    [switch]$Test
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$obj = Join-Path $root 'obj'
$bin = Join-Path $root 'bin'

function Find-VcVars {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $installation = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installation) { return Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat' }
    }
    throw 'No Visual Studio installation with the x64 C++ tools was found. Pass the path of vcvars64.bat with -VcVars.'
}

function Import-MsvcEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }

    $vcvars = if ($VcVars) { $VcVars } else { Find-VcVars }
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat was not found at $vcvars." }

    $environment = & cmd.exe /d /c "`"$vcvars`" >nul && set"
    if ($LASTEXITCODE -ne 0) { throw "vcvars64.bat failed with exit code $LASTEXITCODE." }

    foreach ($line in $environment) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) {
            [Environment]::SetEnvironmentVariable($line.Substring(0, $separator), $line.Substring($separator + 1), 'Process')
        }
    }
}

function Invoke-Tool {
    param([string]$Name, [string[]]$Arguments)
    & $Name @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Name failed with exit code $LASTEXITCODE." }
}

Import-MsvcEnvironment
New-Item -ItemType Directory -Force $obj, $bin | Out-Null

$sources = Get-ChildItem (Join-Path $root 'src\*.c') | ForEach-Object FullName
$objects = $sources | ForEach-Object { Join-Path $obj ([IO.Path]::GetFileNameWithoutExtension($_) + '.obj') }

$compilerFlags = @(
    '/nologo', '/c', '/W4', '/WX', '/std:c17', '/utf-8',
    '/O2', '/Oi', '/GL', '/GS-', '/Gs1000000', '/Zl', '/Zi',
    '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN',
    "/Fo$obj\", "/Fd$obj\QuickPad-compile.pdb"
)
Invoke-Tool cl.exe ($compilerFlags + $sources)

$linkerFlags = @(
    '/nologo', '/NODEFAULTLIB', '/ENTRY:QuickPadEntry', '/SUBSYSTEM:WINDOWS',
    '/LTCG', '/OPT:REF', '/OPT:ICF', '/INCREMENTAL:NO',
    '/DEBUG', "/PDB:$obj\QuickPad.pdb", '/PDBALTPATH:%_PDB%',
    '/STACK:0x100000,0x100000', '/DYNAMICBASE', '/NXCOMPAT', '/HIGHENTROPYVA', '/MANIFEST:NO',
    "/OUT:$bin\QuickPad.exe"
)
$libraries = @('kernel32.lib', 'user32.lib', 'gdi32.lib', 'ntdll.lib')
Invoke-Tool link.exe ($linkerFlags + $objects + $libraries)

Write-Host "Built $bin\QuickPad.exe"

if ($Test) {
    $testObj = Join-Path $obj 'tests'
    New-Item -ItemType Directory -Force $testObj | Out-Null

    $testFlags = @(
        '/nologo', '/W4', '/WX', '/std:c17', '/utf-8', '/O2', '/MT',
        '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', "/I$root\src",
        "/Fo$testObj\", "/Fd$testObj\"
    )

    # Each test program and the sources it tests.
    $unitTests = [ordered]@{
        'text_tests'     = @('text.c')
        'document_tests' = @('document.c')
        'layout_tests'   = @('layout.c')
    }
    foreach ($name in $unitTests.Keys) {
        $testSources = @("$root\tests\$name.c") + ($unitTests[$name] | ForEach-Object { "$root\src\$_" })
        Invoke-Tool cl.exe ($testFlags + $testSources + @("/Fe$bin\$name.exe", '/link', 'user32.lib'))
    }

    foreach ($name in $unitTests.Keys) {
        Write-Host "Running $name"
        & "$bin\$name.exe"
        if ($LASTEXITCODE -ne 0) { throw "$name failed." }
    }
}
