[Setup]
AppName=Alphastudio DRUM SAT 76
AppVersion=1.0.1
AppPublisher=Alphastudio
DefaultDirName={commonpf}\Common Files\VST3\Alphastudio DRUM SAT 76.vst3
DisableDirPage=no 
OutputDir=.\
OutputBaseFilename=DRUM_SAT_76_Windows_Installer
Compression=lzma
SolidCompression=yes

[Files]
Source: "build\DRUM_SAT_76_artefacts\Release\VST3\Alphastudio DRUM SAT 76.vst3\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
