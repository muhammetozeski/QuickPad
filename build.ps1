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
    '/O2', '/Oi', '/GS-', '/Gs1000000', '/Zl', '/Zi',
    '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN',
    "/Fo$obj\", "/Fd$obj\QuickPad-compile.pdb"
)
# The memory functions in nocrt.c stand in for compiler helpers, which whole program optimization
# does not allow, so that file is compiled without /GL.
$helperSources = $sources | Where-Object { (Split-Path $_ -Leaf) -eq 'nocrt.c' }
$programSources = $sources | Where-Object { (Split-Path $_ -Leaf) -ne 'nocrt.c' }
Invoke-Tool cl.exe ($compilerFlags + '/GL' + $programSources)
Invoke-Tool cl.exe ($compilerFlags + $helperSources)

# QuickPadShell.dll runs inside Explorer and is embedded in QuickPad.exe, so it is built first.
$shellObj = Join-Path $obj 'shell'
New-Item -ItemType Directory -Force $shellObj | Out-Null
$shellFlags = @(
    '/nologo', '/c', '/W4', '/WX', '/std:c17', '/utf-8', '/O1', '/Oi', '/GS-', '/Zl', '/Zi',
    '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', "/Fo$shellObj\", "/Fd$shellObj\QuickPadShell-compile.pdb"
)
Invoke-Tool cl.exe ($shellFlags + @("$root\shell\QuickPadShell.c", "$root\src\nocrt.c"))
Invoke-Tool link.exe @(
    '/nologo', '/DLL', '/NOENTRY', '/NODEFAULTLIB', "/DEF:$root\shell\QuickPadShell.def",
    '/OPT:REF', '/OPT:ICF', '/INCREMENTAL:NO', '/DEBUG', "/PDB:$shellObj\QuickPadShell.pdb", '/PDBALTPATH:%_PDB%',
    '/DYNAMICBASE', '/NXCOMPAT', '/HIGHENTROPYVA', "/IMPLIB:$shellObj\QuickPadShell.lib", "/OUT:$bin\QuickPadShell.dll",
    "$shellObj\QuickPadShell.obj", "$shellObj\nocrt.obj", 'kernel32.lib', 'user32.lib', 'ole32.lib', 'uuid.lib', 'ntdll.lib'
)

Invoke-Tool rc.exe @('/nologo', '/i', "$root\res", '/i', $bin, '/fo', "$obj\QuickPad.res", "$root\res\QuickPad.rc")

$linkerFlags = @(
    '/nologo', '/NODEFAULTLIB', '/ENTRY:QuickPadEntry', '/SUBSYSTEM:WINDOWS',
    '/LTCG', '/OPT:REF', '/OPT:ICF', '/INCREMENTAL:NO',
    '/DEBUG', "/PDB:$obj\QuickPad.pdb", '/PDBALTPATH:%_PDB%',
    '/STACK:0x100000,0x100000', '/DYNAMICBASE', '/NXCOMPAT', '/HIGHENTROPYVA', '/MANIFEST:NO',
    "/OUT:$bin\QuickPad.exe"
)
# Libraries only the editor needs are bound on first use in src\lazyload.c and are not linked here.
$libraries = @('kernel32.lib', 'user32.lib', 'gdi32.lib', 'ntdll.lib', 'uuid.lib')
Invoke-Tool link.exe ($linkerFlags + $objects + "$obj\QuickPad.res" + $libraries)

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
        'history_tests'  = @('history.c', 'document.c')
        'search_tests'   = @('search.c')
        'textview_tests' = @('textview.c', 'document.c', 'history.c', 'layout.c', 'search.c')
        'fileio_tests'   = @('fileio.c', 'text.c')
    }
    foreach ($name in $unitTests.Keys) {
        $testSources = @("$root\tests\$name.c") + ($unitTests[$name] | ForEach-Object { "$root\src\$_" })
        Invoke-Tool cl.exe ($testFlags + $testSources + @("/Fe$bin\$name.exe", '/link', 'user32.lib', 'gdi32.lib', 'imm32.lib'))
    }

    # Development tools that are built but not run here.
    foreach ($name in @('snapshot', 'command', 'open_bench')) {
        Invoke-Tool cl.exe ($testFlags + @("$root\tests\$name.c", "/Fe$bin\$name.exe", '/link', 'user32.lib', 'gdi32.lib', 'dwmapi.lib', 'shell32.lib', 'ole32.lib', 'uuid.lib'))
    }

    foreach ($name in $unitTests.Keys) {
        Write-Host "Running $name"
        & "$bin\$name.exe"
        if ($LASTEXITCODE -ne 0) { throw "$name failed." }
    }
}
