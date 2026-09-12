[CmdletBinding()]
param(
    [string[]] $Versions = @('4.27', '5.1', '5.2', '5.3', '5.4', '5.5', '5.6', '5.7', '5.8'),
    [string] $PluginRoot = (Split-Path -Parent $PSScriptRoot),
    [string] $BlueprintLispRoot = 'G:\Github\Blueprint2DSL',
    [string] $HostRoot = (Join-Path $env:LOCALAPPDATA 'AnimBP2FP-CrossVersionTests'),
    [string] $ReportRoot = (Join-Path $env:LOCALAPPDATA 'AnimBP2FP-CrossVersionReports'),
    [string] $TestFilter = 'AnimBP2FP'
)

$ErrorActionPreference = 'Stop'

function Resolve-EngineRoot([string] $Version) {
    $candidates = if ($Version -eq '4.27') {
        @(
            'D:\UE4.27\UE_4.27',
            'C:\Program Files\Epic Games\UE_4.27'
        )
    } else {
        @(
            "C:\Program Files\Epic Games\UE_$Version",
            "D:\UE$Version\UE_$Version"
        )
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'Engine')) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "Unreal Engine $Version was not found. Checked: $($candidates -join ', ')"
}

function Ensure-Junction([string] $Path, [string] $Target) {
    $resolvedTarget = (Resolve-Path -LiteralPath $Target).Path
    if (Test-Path -LiteralPath $Path) {
        $item = Get-Item -LiteralPath $Path -Force
        $currentTarget = @($item.Target)[0]
        if ($item.LinkType -ne 'Junction' -or
            [System.IO.Path]::GetFullPath($currentTarget) -ne [System.IO.Path]::GetFullPath($resolvedTarget)) {
            throw "Existing path is not the expected junction: $Path"
        }
        return
    }
    New-Item -ItemType Junction -Path $Path -Target $resolvedTarget | Out-Null
}

function Invoke-VersionTests([string] $Version) {
    $engineRoot = Resolve-EngineRoot $Version
    $hostProjectRoot = Join-Path $HostRoot "UE$Version"
    $plugins = Join-Path $hostProjectRoot 'Plugins'
    New-Item -ItemType Directory -Force -Path $plugins | Out-Null
    Ensure-Junction (Join-Path $plugins 'AnimBP2FP') $PluginRoot
    Ensure-Junction (Join-Path $plugins 'BlueprintLisp') $BlueprintLispRoot

    $project = Join-Path $hostProjectRoot 'AnimBP2FPTests.uproject'
    $descriptor = [ordered]@{
        FileVersion = 3
        EngineAssociation = $Version
        Category = ''
        Description = 'Generated host project for AnimBP2FP cross-version tests'
        DisableEnginePluginsByDefault = $true
        Plugins = @(
            [ordered]@{ Name = 'AnimBP2FP'; Enabled = $true },
            [ordered]@{ Name = 'BlueprintLisp'; Enabled = $true },
            [ordered]@{ Name = 'ControlRig'; Enabled = $true }
        )
    }
    $descriptor | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $project -Encoding UTF8

    Write-Host "[$Version] Building test host with $engineRoot"
    if ($Version -eq '4.27') {
        $buildTool = Join-Path $engineRoot 'Engine\Binaries\DotNET\UnrealBuildTool.exe'
        & $buildTool UE4Editor Win64 Development "-Project=$project" -WaitMutex -NoHotReload -MaxParallelActions=4
        $editor = Join-Path $engineRoot 'Engine\Binaries\Win64\UE4Editor-Cmd.exe'
    } else {
        $buildTool = Join-Path $engineRoot 'Engine\Build\BatchFiles\Build.bat'
        $buildArguments = @('UnrealEditor', 'Win64', 'Development', "-Project=$project", '-WaitMutex', '-NoHotReload', '-MaxParallelActions=4')
        if ([version]$Version -ge [version]'5.7') {
            $buildArguments += '-CompilerVersion=14.44.35207'
        } elseif ([version]$Version -ge [version]'5.3') {
            $buildArguments += '-CompilerVersion=14.38.33130'
        }
        if ([version]$Version -ge [version]'5.5') {
            $buildArguments += @('-NoUBA', '-NoUBALocal')
        }
        & $buildTool @buildArguments
        $editor = Join-Path $engineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
    }
    if ($LASTEXITCODE -ne 0) {
        throw "UE $Version build failed with exit code $LASTEXITCODE"
    }

    $report = Join-Path $ReportRoot "UE$Version"
    New-Item -ItemType Directory -Force -Path $report | Out-Null
    Write-Host "[$Version] Running $TestFilter automation tests"
    & $editor $project -unattended -nopause -nullrhi -nosplash `
        "-ExecCmds=Automation RunTests $TestFilter;Quit" `
        '-TestExit=Automation Test Queue Empty' `
        "-ReportExportPath=$report" -stdout
    $editorExitCode = $LASTEXITCODE

    $indexPath = Join-Path $report 'index.json'
    if (!(Test-Path -LiteralPath $indexPath)) {
        throw "UE $Version did not produce an automation report (editor exit $editorExitCode)"
    }
    $result = Get-Content -LiteralPath $indexPath -Raw | ConvertFrom-Json
    $total = [int]$result.succeeded + [int]$result.failed + [int]$result.notRun
    if ($total -eq 0 -or [int]$result.failed -ne 0 -or [int]$result.notRun -ne 0 -or $editorExitCode -ne 0) {
        throw "UE $Version tests failed: passed=$($result.succeeded), failed=$($result.failed), notRun=$($result.notRun), editorExit=$editorExitCode"
    }

    [pscustomobject]@{
        Version = $Version
        Passed = [int]$result.succeeded
        Failed = [int]$result.failed
        NotRun = [int]$result.notRun
        Report = $indexPath
    }
}

if (!(Test-Path -LiteralPath $PluginRoot)) {
    throw "AnimBP2FP plugin root does not exist: $PluginRoot"
}
if (!(Test-Path -LiteralPath $BlueprintLispRoot)) {
    throw "BlueprintLisp plugin root does not exist: $BlueprintLispRoot"
}

New-Item -ItemType Directory -Force -Path $HostRoot, $ReportRoot | Out-Null
$summary = foreach ($version in $Versions) {
    Invoke-VersionTests $version
}
$summary | Format-Table -AutoSize
