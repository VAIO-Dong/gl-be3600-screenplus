[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $repositoryRoot 'build\repeater-tests'
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$source = Get-Content (Join-Path $repositoryRoot 'src\system_info.c') -Raw -Encoding UTF8

# Compile the actual samplers with simulated ubus responses on Windows.
# The remainder of system_info.c requires Linux networking/procfs headers.
$samplers = foreach ($name in @('sample_interface', 'sample_ethernet_interface', 'sample_ethernet', 'sample_repeater', 'sample_repeater_cached', 'sample_tethering', 'sample_cellular', 'parse_kmwan_health', 'apply_uplink_health')) {
    $match = [regex]::Match($source, "(?ms)^static (?:void|bool|int) $name\(.*?^\}")
    if (-not $match.Success) { throw "Production sampler not found: $name" }
    $match.Value
}
$preamble = @'
#include <stdio.h>
#include <string.h>
#include "system_info.h"
static int run_line(const char *, char *, unsigned int);
static int ubus_interface_value(const char *, const char *, char *, unsigned int);
static int device_carrier(const char *);
static int ethernet_carrier(const struct uplink_info *);
static struct { unsigned int accent_colour, standby_colour, warning_colour, secondary_colour; }
app_config = { 1, 2, 3, 4 };
'@
$uiSource = Get-Content (Join-Path $repositoryRoot 'src\main.c') -Raw -Encoding UTF8
$colour = [regex]::Match($uiSource, '(?ms)^static unsigned int network_state_colour\(.*?^\}')
if (-not $colour.Success) { throw 'Production network colour mapping not found' }
$samplers += $colour.Value
$testSource = Get-Content (Join-Path $repositoryRoot 'tests\repeater-state.c') -Raw -Encoding UTF8
$generatedPath = Join-Path $buildDirectory 'repeater-state.c'
[IO.File]::WriteAllText($generatedPath, ($preamble + "`n" + ($samplers -join "`n") + "`n" + $testSource), [Text.UTF8Encoding]::new($false))
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $repositoryRoot 'build\cache\zig-global'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $repositoryRoot 'build\cache\zig-tests'
$executable = Join-Path $buildDirectory 'repeater-state.exe'
& (Join-Path $repositoryRoot '.tools\zig-0.15.2\zig.exe') cc -std=c11 -Wall -Wextra -Werror "-I$repositoryRoot\src" $generatedPath -o $executable
if ($LASTEXITCODE -ne 0) { throw 'Repeater test build failed' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'Repeater state tests failed' }
