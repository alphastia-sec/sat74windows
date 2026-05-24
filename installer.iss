[Setup]
AppName=Alphastudio SAT 76
AppVersion=1.0.2
AppPublisher=Alphastudio
AppPublisherURL=https://www.alphastudio.com.pl
DefaultDirName={commoncf64}\VST3\Alphastudio SAT 76.vst3
DisableDirPage=yes
OutputDir=.\
OutputBaseFilename=SAT_76_Windows_Installer
Compression=lzma
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64

[Files]
Source: "build\*_artefacts\Release\VST3\Alphastudio SAT 76.vst3\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Alphastudio SAT 76"; Filename: "{app}"

[Messages]
WelcomeLabel1=Witaj w instalatorze Alphastudio SAT 76
WelcomeLabel2=Ten instalator umieści wtyczkę SAT 76 w systemowym folderze VST3.%n%nZamknij DAW przed kontynuowaniem.
FinishedLabel=Alphastudio SAT 76 została zainstalowana. Uruchom DAW i wykonaj skanowanie wtyczek.
