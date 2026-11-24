param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$sdk = $env:ANDROID_HOME
if (!$sdk) { $sdk = Join-Path $env:LOCALAPPDATA 'Android/Sdk' }
$platform = Join-Path $sdk 'platforms/android-36/android.jar'
$tools = Get-ChildItem (Join-Path $sdk 'build-tools') -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$jdk = Join-Path $env:JAVA_HOME 'bin'
$work = (New-Item -Force -ItemType Directory $OutputDirectory).FullName
foreach ($dir in 'classes','dex') {
    New-Item -Force -ItemType Directory (Join-Path $work $dir) | Out-Null
}
Push-Location $work
try {
    & "$jdk/javac" --release 8 -cp $platform -d classes "$PSScriptRoot/Probe.java"
    & "$jdk/jar" cf classes.jar -C classes .
    & "$tools/d8.bat" --lib $platform --min-api 34 --output dex classes.jar
    if (!(Test-Path -LiteralPath test.p12)) {
        & "$jdk/keytool" -genkeypair -alias probe -keyalg RSA -keysize 2048 -validity 365 -dname 'CN=Kemu Test' -keystore test.p12 -storepass android -keypass android
    }
    foreach ($probe in 'wide','tall') {
        $orientation = if ($probe -eq 'wide') { 'landscape' } else { 'portrait' }
        @"
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="org.kemu.tests.tabs.$probe">
  <uses-sdk android:minSdkVersion="34" android:targetSdkVersion="35" />
  <application android:label="Tab $probe" android:theme="@android:style/Theme.Material.NoActionBar">
    <activity android:name="org.kemu.tests.tabs.Probe" android:screenOrientation="$orientation" android:exported="true">
      <intent-filter><action android:name="android.intent.action.MAIN"/><category android:name="android.intent.category.LAUNCHER"/></intent-filter>
    </activity>
  </application>
</manifest>
"@ | Set-Content AndroidManifest.xml -Encoding utf8
        & "$tools/aapt2.exe" link -I $platform --manifest AndroidManifest.xml -o unsigned.apk
        & "$jdk/jar" uf unsigned.apk -C dex classes.dex
        & "$tools/zipalign.exe" -f 4 unsigned.apk aligned.apk
        & "$tools/apksigner.bat" sign --ks test.p12 --ks-pass pass:android --out "$probe.apk" aligned.apk
    }
} finally {
    Pop-Location
}
