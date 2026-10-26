# 本地 RTSP 测试流基建（testing-guide §6）：MediaMTX + ffmpeg testsrc2 推流
# 用法：
#   start_test_stream.ps1 -Start     # 起流（幂等）
#   start_test_stream.ps1 -Stop      # 停流
#   start_test_stream.ps1 -Probe     # 探测 8554 是否有服务（退出码 0=有）
# 依赖：mediamtx.exe（默认找 $env:TEMP\msys2-setup\mediamtx\，可用 -MediaMtx 指定）
#       ffmpeg（PATH）

param(
    [switch]$Start,
    [switch]$Stop,
    [switch]$Probe,
    [string]$MediaMtx = "$env:TEMP\msys2-setup\mediamtx\mediamtx.exe"
)

$RtspPort = 8554
$StreamUrl = "rtsp://127.0.0.1:$RtspPort/test"

function Test-StreamUp {
    (Test-NetConnection 127.0.0.1 -Port $RtspPort -InformationLevel Quiet -WarningAction SilentlyContinue)
}

if ($Probe -or (-not ($Start -or $Stop))) {
    if (Test-StreamUp) { Write-Output "UP"; exit 0 } else { Write-Output "DOWN"; exit 1 }
}

if ($Stop) {
    Get-CimInstance Win32_Process -Filter "Name='ffmpeg.exe'" |
        Where-Object { $_.CommandLine -like "*$RtspPort*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Get-Process mediamtx -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Write-Output "stopped"
    exit 0
}

# -Start
if (Test-StreamUp) { Write-Output "already up"; exit 0 }
if (-not (Test-Path $MediaMtx)) {
    Write-Error "mediamtx.exe not found: $MediaMtx (use -MediaMtx to specify)"
    exit 2
}
Start-Process -WindowStyle Hidden $MediaMtx -WorkingDirectory (Split-Path $MediaMtx)
Start-Sleep 3
if (-not (Test-StreamUp)) { Write-Error "mediamtx failed to listen on port $RtspPort"; exit 3 }
Start-Process -WindowStyle Hidden ffmpeg -ArgumentList @(
    '-re', '-f', 'lavfi', '-i', 'testsrc2=size=640x480:rate=25',
    '-c:v', 'libx264', '-preset', 'ultrafast', '-tune', 'zerolatency',
    '-pix_fmt', 'yuv420p', '-g', '25', '-sc_threshold', '0',
    '-f', 'rtsp', $StreamUrl)
Start-Sleep 4
if (-not (Test-StreamUp)) { Write-Error "stream setup failed"; exit 4 }
Write-Output "up"
exit 0
