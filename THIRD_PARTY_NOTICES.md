# Third-party components

## Microsoft SysVAD — MIT

Vendored source: `driver/sysvad/`. Copyright Microsoft Corporation. License: `driver/sysvad/LICENSE`.

Origin: https://github.com/microsoft/Windows-driver-samples/tree/2dc3fd3a0cc84a2933f2194e7ec0871584979071/audio/sysvad

OmniSound supplies new endpoint tables (`driver/minipairs.h`) and INF. Local upstream edits:

- `adapter.cpp`: remove registry control that could enable rendered PCM recording; `g_DoNotCreateDataFiles` remains fixed at 1.
- `common.cpp`: permit multiple independent adapter instances; remove the global recording-device assignment and single-instance assertion.
- `EndpointsCommon/MiniportAudioEngineNode.cpp`: guard eight sideband-only code paths so the no-sideband build compiles.

The driver build does not include the sample APOs, keyword adapter DLL, physical Bluetooth/USB sideband devices, or sample endpoint tables.

## nlohmann/json 3.11.3 — MIT

Vendored source: `third_party/nlohmann/json.hpp`. License: `third_party/nlohmann/LICENSE.MIT`.

Origin: https://github.com/nlohmann/json/tree/v3.11.3

SHA-256 of json.hpp: `9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6`.

## Microsoft SDK/WDK tools

Development SDK/WDK tools are not shipped in the product. Build scripts restore version 10.0.28000.2526 to ignored `tools/cache`.

## Microsoft Windows App SDK 1.8.260804001

The unpackaged WinUI 3 application redistributes the Microsoft-signed bootstrapper, XAML resources, and Windows App Runtime installer. Microsoft's license terms are included as licenses/WindowsAppSDK-License.txt in each application package. Use and redistribution are subject to those terms.

## Inno Setup 7.1.0

The standard setup executable uses Inno Setup by Jordan Russell and Martijn Laan.
The pinned official compiler is restored in portable mode for builds; it is not
installed on the target PC. Its license is included as licenses/InnoSetup-License.txt.
Origin: https://jrsoftware.org/ and https://github.com/jrsoftware/issrc/tree/is-7_1_0.
