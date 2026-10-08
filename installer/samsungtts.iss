; Inno Setup script for the Samsung TTS SAPI voices and their voice centre.
;
;   "D:\Program Files\Inno Setup 6\ISCC.exe" installer\samsungtts.iss      ->  dist\SamsungTTS-Setup-<version>.exe
;
; It packages what build.ps1 puts in dist\SamsungTTS. No voice is included: the voice centre downloads them from
; Samsung; a short recorded sample of each (tools\make_samples.py) is included.
; NOTE: samsungtts_sapi.dll is Samsung's engine, translated to native code together with the library's data, so
; the installer this script produces contains Samsung's software. Publishing it is the builder's decision to take.

#define AppVersion "0.2.0"

[Setup]
AppId={{7A4E2B1C-93D6-4F0A-8B5E-1C2D3E4F5A60}
AppName=Samsung TTS voices
AppVersion={#AppVersion}
AppPublisher=samsung-tts project
DefaultDirName={autopf}\SamsungTTS
DefaultGroupName=Samsung TTS voices
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=SamsungTTS-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Samsung TTS voices

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Dirs]
; the voice centre runs as the ordinary user and puts voice packs here
Name: "{app}\data"; Permissions: users-modify
Name: "{app}\data\voice"; Permissions: users-modify

[Files]
Source: "..\dist\SamsungTTS\samsungtts_sapi.dll"; DestDir: "{app}"; Flags: ignoreversion regserver 64bit restartreplace uninsrestartdelete
Source: "..\dist\SamsungTTS\SamsungVoices.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\SamsungTTS\samples\*.wav"; DestDir: "{app}\samples"; Flags: ignoreversion
Source: "..\licenses\bionic-libm-NOTICE.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion

[InstallDelete]
; versions before 0.2.0 kept the engine's library beside the DLL; it is inside the DLL now
Type: files; Name: "{app}\data\libsamsungtts.so"
Type: files; Name: "{app}\data\libm.so"

[Icons]
Name: "{group}\Samsung TTS voices"; Filename: "{app}\SamsungVoices.exe"
Name: "{group}\Uninstall Samsung TTS voices"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\SamsungVoices.exe"; Description: "Open the voice centre to download voices"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; downloaded voice packs
Type: filesandordirs; Name: "{app}\data"
