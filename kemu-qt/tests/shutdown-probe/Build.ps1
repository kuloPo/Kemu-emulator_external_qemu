param(
    [Parameter(Mandatory)][string]$JavaHome,
    [Parameter(Mandatory)][string]$BuildTools,
    [Parameter(Mandatory)][string]$AndroidJar,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
function Check-Exit { if ($LASTEXITCODE -ne 0) { throw "Build command failed: $LASTEXITCODE" } }
New-Item -ItemType Directory -Path "$OutputDirectory\classes", "$OutputDirectory\dex" -Force | Out-Null
$previousJavaHome = $env:JAVA_HOME
try {
    $env:JAVA_HOME = $JavaHome
    & "$JavaHome\bin\javac.exe" -source 8 -target 8 -bootclasspath $AndroidJar -d "$OutputDirectory\classes" "$PSScriptRoot\ProbeReceiver.java"
    Check-Exit
    & "$BuildTools\d8.bat" --lib $AndroidJar --output "$OutputDirectory\dex" "$OutputDirectory\classes\com\kemu\shutdownprobe\ProbeReceiver.class"
    Check-Exit
    & "$BuildTools\aapt2.exe" link -I $AndroidJar --manifest "$PSScriptRoot\AndroidManifest.xml" -o "$OutputDirectory\unsigned.apk"
    Check-Exit
    & "$JavaHome\bin\jar.exe" uf "$OutputDirectory\unsigned.apk" -C "$OutputDirectory\dex" classes.dex
    Check-Exit
    & "$BuildTools\zipalign.exe" -f 4 "$OutputDirectory\unsigned.apk" "$OutputDirectory\aligned.apk"
    Check-Exit
    if (!(Test-Path "$OutputDirectory\test.jks")) {
        & "$JavaHome\bin\keytool.exe" -genkeypair -keystore "$OutputDirectory\test.jks" -storepass android -keypass android -alias test -dname 'CN=Kemu Shutdown Test' -keyalg RSA -validity 3650
        Check-Exit
    }
    & "$BuildTools\apksigner.bat" sign --ks "$OutputDirectory\test.jks" --ks-pass pass:android --key-pass pass:android --out "$OutputDirectory\probe.apk" "$OutputDirectory\aligned.apk"
    Check-Exit
} finally {
    $env:JAVA_HOME = $previousJavaHome
}
