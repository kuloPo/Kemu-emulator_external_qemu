# Offline legal UI checks

Build this standalone Windows target from a Visual Studio developer shell with
CMake, Ninja and Qt 6.5 or newer. Point `CMAKE_PREFIX_PATH` at the Qt SDK and set
`KEMU_LRELEASE_EXECUTABLE` if Qt Linguist is not on PATH.

```powershell
cmake -S kemu-qt/tests/legal -B build/legal-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/legal-tests
```

Place the selected Qt SDK's `bin` directory on PATH and set
`QT_QPA_PLATFORM_PLUGIN_PATH` to its `plugins/platforms` directory. Run:

```powershell
$env:QT_QPA_PLATFORM = 'offscreen'
build/legal-tests/Kemu.exe <staged-installation> <existing-screenshot-directory>
```

The staged installation must contain `NOTICE.txt`, `NOTICE.csv`, and the `legal`
directory installed by CMake, including Android notices exported by the image
copy script. The test uses temporary settings and a Unicode installation path.
It checks both UI languages, offline navigation, full license text, Android
notices, missing files, and rejection of links outside the installation. Plain
text checks cover copyright contacts, literal markup, Unicode, indentation,
blank lines, complete bundled notices, and back/forward navigation. It does not
start a virtual machine or contact the update service.
