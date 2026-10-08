# Licensing

OmniSound is open source under the licenses listed below. A component's own license takes precedence over the repository's MIT license.

| Component | License | Scope |
| --- | --- | --- |
| Original OmniSound code, assets, and documentation | MIT | Project-authored files outside `driver/` and separately licensed third-party components. See [LICENSE](LICENSE). |
| OmniSound driver and vendored Microsoft SysVAD source | Microsoft Public License (MS-PL) | All files under `driver/`, including OmniSound driver-specific contributions and modifications. See [driver/sysvad/LICENSE](driver/sysvad/LICENSE); packaged as `licenses/SysVAD-MS-PL.txt`. |
| nlohmann/json 3.11.3 | MIT | `third_party/nlohmann/json.hpp`. See [third_party/nlohmann/LICENSE.MIT](third_party/nlohmann/LICENSE.MIT). |
| Microsoft Windows App SDK runtime/bootstrapper | Microsoft terms | Redistributable files in application packages. The applicable license is included as `licenses/WindowsAppSDK-License.txt` in each package. |
| Inno Setup 7.1.0 | Inno Setup License | The setup program/compiler used to produce the installer. The applicable license is included as `licenses/InnoSetup-License.txt` in each package. |

The Windows SDK/WDK and build tools are restored into the ignored `tools/cache/` directory and are not part of the source repository. Their own terms apply when obtaining and using those tools.

The MS-PL is an open-source license. Its source-distribution terms mean the driver tree must retain the MS-PL rather than being relicensed under the repository's MIT license. This does not prevent OmniSound from being open source under these clearly scoped licenses.
