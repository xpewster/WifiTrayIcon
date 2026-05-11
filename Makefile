!IFDEF debug
CFLAGS_OPT = /Od /Zi /DEBUG
LFLAGS_OPT = /DEBUG
!ELSE
CFLAGS_OPT = /O2 /GL
LFLAGS_OPT = /LTCG /OPT:REF /OPT:ICF
!ENDIF

CFLAGS = /nologo /EHsc /std:c++17 /W4 /DUNICODE /D_UNICODE $(CFLAGS_OPT)
LIBS   = user32.lib shell32.lib wlanapi.lib iphlpapi.lib ws2_32.lib ole32.lib

all: WifiTrayIcon.exe

WifiTrayIcon.exe: wifi_tray_icon.cpp app.res resource.h
	cl $(CFLAGS) wifi_tray_icon.cpp app.res \
	   /link /SUBSYSTEM:WINDOWS $(LFLAGS_OPT) $(LIBS) /OUT:WifiTrayIcon.exe

app.res: app.rc resource.h app.manifest
	rc /nologo /fo app.res app.rc

clean:
	-del *.obj *.res *.exe *.pdb *.ilk 2>nul
