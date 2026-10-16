# Kemu Qt translations

The Qt UI uses English source strings and the `Kemu` translation context.
Simplified Chinese translations are maintained in `kemu_zh_CN.ts`, which can
be edited with Qt Linguist. `LanguageNames` contains native language names
that remain readable when the interface language changes.

Use `QCoreApplication::translate("Kemu", "English source text")` for UI text.
For deferred strings such as toolbar definitions, mark the source with
`QT_TRANSLATE_NOOP("Kemu", "English source text")`, then translate it when
displaying it. Preserve placeholders such as `%1` and `%2` in translations.
English uses Qt's source-text fallback; a separate English catalog is unnecessary.

Install Qt Linguist tools on the build host. Put `lrelease` and `lupdate` on
PATH, or configure CMake with their absolute paths:

```text
-DKEMU_LRELEASE_EXECUTABLE=<Qt>/bin/lrelease.exe
-DKEMU_LUPDATE_EXECUTABLE=<Qt>/bin/lupdate.exe
```

After adding or changing source strings, run:

```text
cmake --build <build-directory> --target kemu-update-translations
```

Review and complete new translations in the TS file. Normal builds compile it
with `lrelease -nounfinished` and embed the QM catalog using Qt resources in
both the emulator and settings tests. No external QM file is needed at runtime.
Commit the TS source, not generated QM or resource C++ files.

`UiLanguage.cpp` owns the QTranslator and installs/removes it when the saved
language changes. The existing UI refresh callbacks retranslate visible text.
`UiLanguage.h` retains the first-launch language selection and persistence API.

This catalog covers Kemu's Qt window chrome, settings, update dialogs and mute
tooltips. The standalone Win32 launcher/updater and upstream emulator UI are
separate components and are not included in this catalog.
