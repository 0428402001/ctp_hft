# ============================================================================
#  launch_env.ps1 - start the four ctp_hft processes for ONE environment.
#
#  Runs under the Windows Task Scheduler service, so the processes it creates
#  live outside any interactive job object and keep running after the shell /
#  agent session that triggered them has gone away.
#
#  Called by tools\guard.py through the scheduled task "ctp_hft_launch".
#
#  Environment selection:
#    1. -Env day|724            explicit
#    2. flow\.wantenv          what the supervisor asked for
#    3. local clock            09:00-16:00 -> day, otherwise -> 724
# ============================================================================
param(
    [ValidateSet('auto', 'day', '724')]
    [string]$Env = 'auto',

    # full     = cold start all four processes (environment switch)
    # strategy = swap strategy_proc.exe only; md/trader/query keep their
    #            sessions, so no front reconnect and no 4097 throttling risk
    [ValidateSet('auto', 'full', 'strategy')]
    [string]$Mode = 'auto'
)

$ErrorActionPreference = 'Stop'

$root = 'D:\quant\ctp_hft'
$bin = Join-Path $root 'build\bin_simnow'

function Resolve-Env([string]$requested) {
    if ($requested -ne 'auto') { return $requested }
    # A hint from the supervisor is only trusted if it is fresh; otherwise an
    # old .wantenv (e.g. "724" written last night) would win a boot at 10:00.
    $wantFile = Join-Path $root 'flow\.wantenv'
    if (Test-Path $wantFile) {
        $age = (Get-Date) - (Get-Item $wantFile).LastWriteTime
        if ($age.TotalMinutes -le 5) {
            $v = (Get-Content $wantFile -Raw).Trim()
            if ($v -eq 'day' -or $v -eq '724') { return $v }
        }
    }
    $m = (Get-Date).Hour * 60 + (Get-Date).Minute
    if ($m -ge 540 -and $m -lt 960) { return 'day' } else { return '724' }
}

$envName = Resolve-Env $Env
$cfgName = if ($envName -eq 'day') { 'config_simnow.ini' } else { 'config_simnow724.ini' }
$cfg = Join-Path $root "config\$cfgName"

function Write-LaunchLog([string]$msg) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $msg
    Add-Content -Path (Join-Path $root 'log\launcher.log') -Value $line -Encoding utf8
}

if (-not (Test-Path $cfg)) {
    Write-LaunchLog "ABORT config not found: $cfg"
    throw "config not found: $cfg"
}

# ---- stop everything first (the shared-memory bus is single-tenant) ---------
#  "strategy" mode only recycles the strategy process: the other three keep
#  their CTP sessions, which is both faster and kinder to the front.
$runMode = $Mode
if ($runMode -eq 'auto') {
    $modeFile = Join-Path $root 'flow\.launchmode'
    $runMode = 'full'
    if (Test-Path $modeFile) {
        $age = (Get-Date) - (Get-Item $modeFile).LastWriteTime
        if ($age.TotalMinutes -le 5) {
            $v = (Get-Content $modeFile -Raw).Trim()
            if ($v -eq 'strategy' -or $v -eq 'full') { $runMode = $v }
        }
    }
}

if ($runMode -eq 'strategy') {
    Get-Process -Name 'strategy_proc' -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    $exe = Join-Path $bin 'strategy_proc.exe'
    $cliArgs = @('--config', $cfg, '--mode', 'live', '--no-console')
    $proc = Start-Process -FilePath $exe -ArgumentList $cliArgs -WorkingDirectory $root `
                          -WindowStyle Hidden -PassThru
    Write-LaunchLog ("strategy-only restart: pid={0} env={1}" -f $proc.Id, $envName)
    Set-Content -Path (Join-Path $root 'flow\.envmode') -Value $envName -Encoding utf8
    exit 0
}

foreach ($p in 'strategy_proc', 'query_proc', 'trader_proc', 'md_proc', 'monitor') {
    Get-Process -Name $p -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds 2

# ---- wipe the bus -----------------------------------------------------------
& (Join-Path $bin 'shm_reset.exe') | Out-Null
Start-Sleep -Seconds 1

# ---- cold start, market data first -----------------------------------------
$order = @(
    @{ exe = 'md_proc.exe';       wait = 3 },
    @{ exe = 'trader_proc.exe';   wait = 3 },
    @{ exe = 'query_proc.exe';    wait = 3 },
    @{ exe = 'strategy_proc.exe'; wait = 0 }
)

foreach ($step in $order) {
    $exe = Join-Path $bin $step.exe
    $cliArgs = @('--config', $cfg, '--mode', 'live', '--no-console')
    $proc = Start-Process -FilePath $exe -ArgumentList $cliArgs -WorkingDirectory $root `
                          -WindowStyle Hidden -PassThru
    Write-LaunchLog ("started {0} pid={1} env={2}" -f $step.exe, $proc.Id, $envName)
    if ($step.wait -gt 0) { Start-Sleep -Seconds $step.wait }
}

# ---- record which environment is live --------------------------------------
#  (.guard_state.json is owned by tools\guard.py - do not touch it here)
Set-Content -Path (Join-Path $root 'flow\.envmode') -Value $envName -Encoding utf8
Write-LaunchLog "environment '$envName' is live ($cfgName)"
