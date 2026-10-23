param(
    [Parameter(Mandatory=$true)][string]$Sdk,
    [Parameter(Mandatory=$true)][string]$Jdk,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$BuildTools = '36.0.0',
    [string]$Platform = 'android-36'
)
$ErrorActionPreference = 'Stop'
$tools = Join-Path $Sdk "build-tools\$BuildTools"
$androidJar = Join-Path $Sdk "platforms\$Platform\android.jar"
$classes = Join-Path $Output 'classes'
$dex = Join-Path $Output 'dex'
New-Item -ItemType Directory -Path $classes,$dex -Force | Out-Null
& "$Jdk\bin\javac.exe" -source 8 -target 8 -classpath $androidJar -d $classes "$PSScriptRoot\RotationActivity.java"
if ($LASTEXITCODE) { throw 'Java compilation failed.' }
$classFiles = @(Get-ChildItem -LiteralPath $classes -Recurse -Filter '*.class' | ForEach-Object FullName)
& "$tools\d8.bat" --lib $androidJar --output $dex $classFiles
if ($LASTEXITCODE) { throw 'Dex compilation failed.' }
& "$tools\aapt2.exe" link -o "$Output\unsigned.apk" --manifest "$PSScriptRoot\AndroidManifest.xml" -I $androidJar
if ($LASTEXITCODE) { throw 'APK link failed.' }
& "$Jdk\bin\jar.exe" uf "$Output\unsigned.apk" -C $dex classes.dex
if ($LASTEXITCODE) { throw 'APK assembly failed.' }
& "$tools\zipalign.exe" -f 4 "$Output\unsigned.apk" "$Output\aligned.apk"
if ($LASTEXITCODE) { throw 'APK alignment failed.' }
$key = Join-Path $Output 'test-only.keystore'
if (-not (Test-Path -LiteralPath $key)) {
    & "$Jdk\bin\keytool.exe" -genkeypair -keystore $key -storepass android -keypass android -alias test -keyalg RSA -validity 3650 -dname 'CN=Rotation Test'
    if ($LASTEXITCODE) { throw 'Test key generation failed.' }
}
& "$tools\apksigner.bat" sign --ks $key --ks-pass pass:android --key-pass pass:android --out "$Output\rotation-probe.apk" "$Output\aligned.apk"
if ($LASTEXITCODE) { throw 'APK signing failed.' }
