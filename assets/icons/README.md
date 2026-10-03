# FloraGPA application icon

`FloraGPA.svg` is the original user-supplied artwork, preserved without edits.
`FloraGPA.ico` contains 16, 20, 24, 32, 40, 48, 64, 96, 128 and 256 pixel images.
The ICO is embedded in the Windows executable and in Qt's resource system for
window/title-bar/taskbar icons. Both files belong in Git.

To regenerate after editing the SVG (development only, requires Qt Svg):

```powershell
cmake -S tools/icon -B build/icon -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=D:/Qt/6.11.2/msvc2022_64
cmake --build build/icon --config Release
$env:PATH = "D:/Qt/6.11.2/msvc2022_64/bin;" + $env:PATH
./build/icon/Release/FloraIconGenerator.exe assets/icons/FloraGPA.svg assets/icons/FloraGPA.ico
```

The converter renders the unchanged SVG at four times each target resolution
and downsamples for antialiasing. It does not run during normal application builds;
application builds need neither Python nor the Qt Svg module.
