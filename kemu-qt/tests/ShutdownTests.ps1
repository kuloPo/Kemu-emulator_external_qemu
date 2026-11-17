param(
    [Parameter(Mandatory)][string]$RuntimeRoot,
    [Parameter(Mandatory)][string]$Adb,
    [Parameter(Mandatory)][string]$ProbeApk,
    [Parameter(Mandatory)][string]$WorkDirectory
)
$ErrorActionPreference = 'Stop'
if (Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue) { throw 'Close existing Kemu instances before this test' }
if (Test-Path $WorkDirectory) { throw 'Use a new work directory for isolated guest data' }
New-Item -ItemType Directory -Path "$WorkDirectory\local\Kemu\Kemu\avd" -Force | Out-Null
$WorkDirectory = (Resolve-Path $WorkDirectory).Path
$requestPath = "$WorkDirectory\request.txt"
$previousLocal = $env:LOCALAPPDATA
$previousRequest = $env:KEMU_SHUTDOWN_TEST_REQUEST

function Wait-Boot {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $state = & $Adb -s emulator-5554 shell getprop sys.boot_completed 2>$null
        if ($state -eq '1') { return }
        Start-Sleep -Milliseconds 500
    } while ($watch.Elapsed.TotalSeconds -lt 60)
    throw 'Guest boot timed out'
}
function Start-Guest {
    Start-Process -FilePath "$RuntimeRoot\Kemu.exe" -WindowStyle Hidden
    Wait-Boot
}
function Probe($action, $token) {
    $output = & $Adb -s emulator-5554 shell am broadcast -a "com.kemu.shutdownprobe.$action" -n com.kemu.shutdownprobe/.ProbeReceiver --es token $token
    $output | Set-Content "$WorkDirectory\$token-$action.txt"
    if (($output -join "`n") -notmatch ('result=0, data="' + $token + '"')) { throw "$action failed: $output" }
}
function Report($message) { $message | Tee-Object -Append "$WorkDirectory\results.txt" }
function Request-Close($engine, $action) {
    [IO.File]::WriteAllText($requestPath, "$($engine.Id) $action")
}
function Check-Exit($engine) {
    if (!$engine.WaitForExit(55000)) { throw 'Guest did not finish shutting down' }
    if ($engine.ExitCode -ne 0) { throw "Engine exit code $($engine.ExitCode)" }
    if (!(Test-Path "$requestPath.result")) { throw 'GUI test hook did not run; build with KEMU_GUI_TESTS=ON' }
}

try {
    $env:LOCALAPPDATA = "$WorkDirectory\local"
    $env:KEMU_SHUTDOWN_TEST_REQUEST = $requestPath
    [IO.File]::WriteAllText("$WorkDirectory\local\Kemu\Kemu\avd\performance.ini", "[performance]`ncores=4`nramGb=4`nresolution=0`nfpsMode=1`n")
    Start-Guest
    & $Adb -s emulator-5554 install --no-incremental $ProbeApk
    if ($LASTEXITCODE) { throw 'Probe installation failed' }
    $flags = & $Adb -s emulator-5554 shell 'su 0 aflags list'
    $flags | Select-String sharedpreferences_skip_fsync | Set-Content "$WorkDirectory\fsync-flag.txt"
    foreach ($action in @('close', 'restart', 'restart')) {
        $token = "$action-" + [guid]::NewGuid().ToString('N')
        $engine = Get-Process qemu-system-x86_64
        $null = $engine.Handle
        Probe WRITE $token
        $watch = [Diagnostics.Stopwatch]::StartNew()
        Request-Close $engine $action
        Check-Exit $engine
        $seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 2)
        if ($action -eq 'close') { Start-Guest } else { Wait-Boot }
        Probe CHECK $token
        $size = & $Adb -s emulator-5554 shell wm size
        Report "$action PASS: shutdown=$seconds seconds; SharedPreferences intact; $size"
    }
    & $Adb -s emulator-5554 push "$PSScriptRoot\shutdown-probe\pause-adbd.sh" /data/local/tmp/kemu-pause-adbd.sh
    if ($LASTEXITCODE) { throw 'Cannot stage ADB pause fixture' }
    $engine = Get-Process qemu-system-x86_64
    $null = $engine.Handle
    Probe WRITE delayed-close
    & $Adb -s emulator-5554 shell 'su 0 sh /data/local/tmp/kemu-pause-adbd.sh'
    if ($LASTEXITCODE) { throw 'Cannot schedule ADB resume' }
    Start-Sleep -Seconds 2
    Request-Close $engine close
    if ($engine.WaitForExit(22000)) { throw 'Guest was terminated while ADB was stalled' }
    Check-Exit $engine
    Start-Guest
    Probe CHECK delayed-close
    Report 'ADB pause PASS: waited beyond 20 seconds, then powered off with SharedPreferences intact'
    $engine = Get-Process qemu-system-x86_64
    $null = $engine.Handle
    Request-Close $engine close
    Check-Exit $engine
} finally {
    $env:LOCALAPPDATA = $previousLocal
    $env:KEMU_SHUTDOWN_TEST_REQUEST = $previousRequest
}
