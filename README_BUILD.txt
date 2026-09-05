WireScope Native v2.0
=====================
Made by Exbadr
Discord: 5c5b

WHAT THIS VERSION IS
--------------------
This is a native Windows C++ rewrite. It does not use .NET, WPF, SharpPcap,
or PacketDotNet. It loads the installed Npcap wpcap.dll dynamically at runtime.

EXPECTED RELEASE FOLDER
-----------------------
WireScope.exe
WireScope.Core.dll
WireScope.Network.dll
WireScope.Filters.dll
WireScope.Geo.dll

No .NET runtimeconfig/deps files are used.
The Release build uses the static MSVC runtime (/MT), so the app does not need
the Visual C++ redistributable to be installed separately.

REQUIREMENTS TO BUILD
---------------------
1. Windows 10/11 x64
2. Visual Studio 2022
3. Workload: Desktop development with C++
4. Npcap does NOT need an SDK for this project. The program uses the installed
   Npcap DLL dynamically.

BUILD
-----
Double-click BUILD_RELEASE.bat

The final files will be placed in the Release folder.

FEATURES IN THIS NATIVE PROJECT
-------------------------------
- Native Win32 dark UI
- Dark Windows title bar
- Adapter selection
- Incoming-only capture by default
- TCP / UDP / HTTP / HTTPS / DNS / QUIC presets
- Discord voice/process-aware UDP filter using Windows socket ownership
- Remote-port ranges and invert filtering
- Public IP / local-noise filtering
- Packet aggregation, packet count, bytes, first/last seen
- IP geolocation, city/region/country, owner/ASN
- Real country flags downloaded once and cached in LocalAppData
- Search
- CSV export
- Double-click a row to copy its IP
- Settings: accent/text/background colors, Windows font, background image,
  image opacity, Geo lookup toggle
- Settings saved in %LOCALAPPDATA%\WireScope\settings.ini
- Made by Exbadr / Discord 5c5b

NOTES
-----
- Npcap must be installed on the PC that runs WireScope.
- Capture may require running WireScope as Administrator depending on Npcap
  installation/security settings.
- Geo/ASN information is approximate network ownership/location data, not a
  person's exact physical location.
- The native DLLs are compiled machine code and Release builds contain no PDBs.
  /O2, whole-program optimization and link-time optimization are enabled. This
  makes the original C++ source substantially harder to reconstruct than .NET
  IL, but no compiled program can be made impossible to reverse engineer.
