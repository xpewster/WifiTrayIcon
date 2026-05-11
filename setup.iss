[Setup]
AppName=WifiTrayIcon
AppVersion=1.0.1
AppPublisher=xpewster
AppPublisherURL=https://github.com/xpewster/WifiTrayIcon
DefaultDirName={localappdata}\WifiTrayIcon
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputBaseFilename=WifiTrayIcon-Setup-{#SetupSetting("AppVersion")}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=classic
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
DisableDirPage=auto
UninstallDisplayIcon={app}\WifiTrayIcon.exe

[Files]
Source: "build\WifiTrayIcon.exe"; DestDir: "{app}"; Flags: ignoreversion

[Tasks]
Name: "autostart"; Description: "Start with Windows"; GroupDescription: "Startup options:"

[Icons]
Name: "{userstartup}\WifiTrayIcon"; Filename: "{app}\WifiTrayIcon.exe"; \
    Tasks: autostart

[Run]
Filename: "{app}\WifiTrayIcon.exe"; Description: "Launch WifiTrayIcon"; \
    Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM WifiTrayIcon.exe"; \
    Flags: runhidden; RunOnceId: "killproc"