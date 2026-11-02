<#
.SYNOPSIS
    安装 / 卸载 / 查看 EPD 墨水屏天气同步（随电脑开机自动运行）。

.DESCRIPTION
    采用「当前用户登录时启动的隐藏任务」方案，原因见 README：
    BLE(WinRT) 需要用户会话上下文，SYSTEM 服务(Session 0)拿不到蓝牙设备。

.EXAMPLE
    .\install_task.ps1              # 安装并立即启动
    .\install_task.ps1 -City 上海    # 安装并指定城市
    .\install_task.ps1 -Status      # 查看运行状态与最近日志
    .\install_task.ps1 -Uninstall   # 卸载
#>
[CmdletBinding()]
param(
    [string]$City,
    [double]$Interval = 60,
    [switch]$Uninstall,
    [switch]$Status,
    [switch]$NoStart
)

$ErrorActionPreference = 'Stop'
$TaskName = 'EPD-Weather-Sync'
$BaseDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$PyExe    = Join-Path $BaseDir '.venv\Scripts\pythonw.exe'
$PyCon    = Join-Path $BaseDir '.venv\Scripts\python.exe'
$Script   = Join-Path $BaseDir 'epd_sync.py'
$Config   = Join-Path $BaseDir 'config.json'
$LogFile  = Join-Path $BaseDir 'logs\epd-sync.log'

function Write-Head($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }

# ---------------------------------------------------------------- 状态
if ($Status) {
    Write-Head "任务状态"
    $t = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    if (-not $t) {
        Write-Host "未安装（找不到计划任务 $TaskName）" -ForegroundColor Yellow
    } else {
        $i = Get-ScheduledTaskInfo -TaskName $TaskName
        [pscustomobject]@{
            状态       = $t.State
            上次运行   = $i.LastRunTime
            上次结果   = $i.LastTaskResult
            下次运行   = $i.NextRunTime
        } | Format-List

        $proc = Get-CimInstance Win32_Process -Filter "Name='pythonw.exe'" -ErrorAction SilentlyContinue |
                Where-Object { $_.CommandLine -like "*epd_sync.py*" }
        if ($proc) {
            Write-Host "进程在跑：PID $($proc.ProcessId -join ', ')" -ForegroundColor Green
            $ws = (Get-Process -Id $proc.ProcessId -ErrorAction SilentlyContinue).WorkingSet64
            if ($ws) { Write-Host ("内存占用：{0:N1} MB" -f ($ws / 1MB)) }
        } else {
            Write-Host "进程未在运行" -ForegroundColor Yellow
        }
    }

    Write-Head "最近日志（末尾 25 行）"
    if (Test-Path $LogFile) { Get-Content $LogFile -Tail 25 } else { Write-Host "暂无日志" }
    return
}

# ---------------------------------------------------------------- 卸载
if ($Uninstall) {
    Write-Head "卸载"
    $t = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    if ($t) {
        Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
        Write-Host "已删除计划任务 $TaskName" -ForegroundColor Green
    } else {
        Write-Host "计划任务本就不存在"
    }
    Get-CimInstance Win32_Process -Filter "Name='pythonw.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like "*epd_sync.py*" } |
        ForEach-Object {
            Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
            Write-Host "已结束进程 PID $($_.ProcessId)"
        }
    Write-Host "`n配置文件与日志保留在：$BaseDir" -ForegroundColor DarkGray
    return
}

# ---------------------------------------------------------------- 前置检查
Write-Head "环境检查"

if (-not (Test-Path $PyExe)) {
    Write-Host "缺少虚拟环境：$PyExe" -ForegroundColor Red
    Write-Host "请先执行：`n  cd `"$BaseDir`"`n  python -m venv .venv`n  .\.venv\Scripts\python.exe -m pip install bleak httpx -i https://pypi.tuna.tsinghua.edu.cn/simple"
    exit 1
}
Write-Host "Python : $(& $PyCon --version 2>&1)"

# 蓝牙适配器
$bt = Get-PnpDevice -Class Bluetooth -ErrorAction SilentlyContinue |
      Where-Object { $_.FriendlyName -match 'Bluetooth' -and $_.Status -eq 'OK' }
if (-not $bt) {
    Write-Host "警告：没检测到正常的蓝牙适配器，同步会失败" -ForegroundColor Yellow
} else {
    Write-Host "蓝牙   : OK"
}

# ---------------------------------------------------------------- 写配置
if ($City -or -not (Test-Path $Config)) {
    $cfg = @{}
    if (Test-Path $Config) {
        try { $cfg = Get-Content $Config -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable } catch { $cfg = @{} }
    }
    if (-not $cfg) { $cfg = @{} }
    if (-not $cfg.ContainsKey('city'))     { $cfg['city'] = 'Hangzhou' }
    if (-not $cfg.ContainsKey('language')) { $cfg['language'] = 'zh' }
    $cfg['interval_minutes'] = $Interval
    if ($City) { $cfg['city'] = $City }
    $json = $cfg | ConvertTo-Json -Depth 6
    [System.IO.File]::WriteAllText($Config, $json, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "配置已写入：$Config（城市=$($cfg['city'])，间隔=$($cfg['interval_minutes']) 分钟）"
}

# ---------------------------------------------------------------- 先跑一次验证
Write-Head "试跑一次（验证链路）"
Write-Host "如果这一步失败，任务装了也不会工作，先解决它。`n"
& $PyCon $Script --once
$testOk = ($LASTEXITCODE -eq 0)
if (-not $testOk) {
    Write-Host "`n试跑失败（退出码 $LASTEXITCODE）" -ForegroundColor Yellow
    Write-Host "常见原因：墨水屏没上电 / 正被手机连着 / 固件还没重新烧录。" -ForegroundColor Yellow
    $ans = Read-Host "仍要继续安装吗？(y/N)"
    if ($ans -notmatch '^[Yy]') { Write-Host '已取消'; return }
} else {
    Write-Host "`n试跑成功，墨水屏应该已经刷新了" -ForegroundColor Green
}

# ---------------------------------------------------------------- 注册任务
Write-Head "注册开机任务"

if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
}

$action = New-ScheduledTaskAction -Execute $PyExe -Argument "`"$Script`"" -WorkingDirectory $BaseDir

# 两个触发器：
#   ① 登录时启动（主入口）
#   ② 每 30 分钟重复一次（自愈）：配合 -MultipleInstances IgnoreNew，
#      进程活着就忽略本次触发，进程挂了就顺手拉起来，不用等下次登录。
$triggerLogon  = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
$triggerRepeat = New-ScheduledTaskTrigger -Once -At ((Get-Date).AddMinutes(2)) `
    -RepetitionInterval (New-TimeSpan -Minutes 30)
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -RestartCount 3 `
    -RestartInterval (New-TimeSpan -Minutes 2) `
    -ExecutionTimeLimit (New-TimeSpan -Days 0) `
    -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" `
    -LogonType Interactive -RunLevel Limited

Register-ScheduledTask -TaskName $TaskName -Action $action `
    -Trigger @($triggerLogon, $triggerRepeat) `
    -Settings $settings -Principal $principal `
    -Description 'ESP32 墨水屏天气站 BLE 自动同步（用户登录后后台运行）' | Out-Null

Write-Host "已注册计划任务：$TaskName" -ForegroundColor Green
Write-Host "  触发：登录时 + 每 30 分钟自愈检查（进程活着会忽略）"

if (-not $NoStart) {
    Start-ScheduledTask -TaskName $TaskName
    Start-Sleep -Seconds 3
    $proc = Get-CimInstance Win32_Process -Filter "Name='pythonw.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.CommandLine -like "*epd_sync.py*" }
    if ($proc) { Write-Host "已启动，PID $($proc.ProcessId -join ', ')" -ForegroundColor Green }
    else { Write-Host "已注册，但进程未立即出现（看日志确认）" -ForegroundColor Yellow }
}

Write-Head "下一步"
Write-Host @"
查看状态 :  .\install_task.ps1 -Status
看日志   :  Get-Content "$LogFile" -Wait
改城市   :  编辑 $Config 里的 "city"，然后 .\install_task.ps1（重装）或重启任务
卸载     :  .\install_task.ps1 -Uninstall
"@ -ForegroundColor DarkGray
