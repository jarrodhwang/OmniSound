# OmniSound Pro: free local testing and signing

## The $0 answer

You do **not** need to buy a certificate to use OmniSound Lite. Lite has no kernel
driver, installs for the current user, and does not require driver signing. Use the
existing `dist/OmniSound-Setup.exe`, choose **Lite**, accept the agreement, and install.
This is the only current route that costs $0 and keeps normal Windows Secure Boot and
driver-signing protections enabled.

OmniSound Pro includes a kernel-mode virtual audio driver. For a one-PC experiment,
this repository has a local test-signing path that creates a private, self-signed test
certificate at no charge. Windows will only load that test driver when **Secure Boot
is disabled** and **TESTSIGNING mode is enabled**, followed by a restart. Windows shows
a Test Mode watermark. This lowers boot/driver security, may require suspending
BitLocker before changing firmware settings, and is not appropriate for a computer
where you want to preserve the current security configuration. The installer checks
these prerequisites; it will refuse to install the Pro test package otherwise. There
is no supported switch that makes this self-signed Pro driver install as a normal,
trusted Windows driver while keeping those protections enabled.

The self-signed certificate establishes trust only on the test PC after it is
installed there. It does **not** make Windows or other people's PCs trust OmniSound,
and the test package must not be redistributed. Building a local test package is free;
Microsoft's official driver signing documentation confirms that test-signed drivers
need TESTSIGNING enabled and that Secure Boot policy can block enabling it. See
[Microsoft: TESTSIGNING boot option](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option)
and [Microsoft: driver signing policy](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/driver-signing).

## Free Pro test steps (only on a dedicated test PC)

These steps intentionally change boot security. Do not follow them on your everyday
PC if you are unwilling to turn off Secure Boot. Back up the BitLocker recovery key
before any firmware change; follow your PC maker's instructions for suspending and
resuming BitLocker. Keep the machine offline from sensitive work while test mode is on.

1. Build OmniSound and its driver using the repository's pinned Visual Studio/WDK
   setup, or use the development build artifacts already produced in `build/`.
2. Run PowerShell as your normal developer account and create the self-signed local
   test package:

   ```powershell
   .\tools\Prepare-LocalTestPackage.ps1
   ```

   The script places a timestamped ZIP in `dist/`, creates a two-year test certificate
   in the current user's certificate store, signs the local app binaries and driver
   catalog, and exports the public certificate inside the package. Keep the private
   key on this PC. This certificate is for local testing only.
3. If you decide to proceed on a dedicated test PC, use its UEFI setup to turn off
   Secure Boot. If BitLocker is enabled, first suspend protection and ensure you have
   the recovery key. Do not guess at these firmware steps; use the PC manufacturer's
   instructions.
4. Open **Terminal (Admin)** and enable Windows test signing:

   ```powershell
   bcdedit /set testsigning on
   ```

   Restart Windows. Verify that the desktop displays **Test Mode**. If Windows says
   the setting is protected by Secure Boot policy, stop: Secure Boot is still active.
5. Extract the generated `OmniSound-0.2.0-local-test-*.zip` and run
   `OmniSoundSetup.exe` from the extracted folder. Choose **Pro** and install. The
   installer verifies the test signatures, checks Secure Boot and TESTSIGNING, trusts
   this test certificate on this PC, then stages the driver and installs the app.
6. In OmniSound Pro, create a group, choose physical outputs, and save. Select
   `OmniSound · <group>` in Windows Sound settings. Confirm the virtual endpoint and
   playback on the real devices. This is a local installation check, not proof of
   measured acoustic synchronization or release readiness.
7. When done, uninstall **OmniSound Pro** from Windows Installed apps. Then, in
   **Terminal (Admin)**, disable test mode:

   ```powershell
   bcdedit /set testsigning off
   ```

   Restart Windows. Re-enable Secure Boot in UEFI, then resume BitLocker protection.
   Confirm Test Mode watermark is gone and Secure Boot/BitLocker are back to their
   original state. If Windows blocks test mode due to firmware policy, do not try to
   bypass that policy on your everyday PC; use Lite instead.

`tools/Prepare-LocalTestPackage.ps1` performs packaging and signing only. It does not
change Secure Boot, BCD settings, or install the driver. Never run a random script or
certificate from a third party to bypass Windows signing policy.

## What costs money, and when

Nothing in the local self-signed test process requires purchasing an EV certificate.
However, **publicly distributing Pro as a normally trusted kernel driver is a separate
release process**. Microsoft's current Windows Hardware Developer Program enrollment
requires an EV certificate and organizational eligibility, followed by Microsoft
submission and the applicable testing/certification. That is not needed for Lite or
for a test-mode-only Pro experiment. If you want to spend nothing, skip these release
steps and do not buy a certificate.

Official references for any future release: [Microsoft Hardware Developer Program
enrollment](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/hardware-program-register),
[certificate management](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-cert-manage),
and [driver signing offerings](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings).
An EV certificate or a Microsoft test/attestation signature alone does not establish
that the finished product has passed the full retail driver certification route.

## Current installer and evidence boundary

`dist/OmniSound-Setup.exe` is the development installer. Lite can be installed from it;
its Pro choice is intentionally unavailable until a Microsoft-signed driver and
publisher-signed release package exist. The local test package is a separate ZIP and
uses the local-test install mode described above.

The Pro audio implementation supports 48kHz stereo, side 5.1 and 7.1 PCM16/24/32;
speaker positioning is PCM routing with distance attenuation, not Dolby object
decoding or HRTF. WASAPI shared-mode buffers and device/Bluetooth latency prevent a
zero-latency guarantee. Successful build/package checks do not prove driver loading,
live physical playback, measured 1ms acoustic alignment, Bluetooth stability, HLK, or
general Windows distribution. Run those only on suitable test hardware and report
each kind of evidence separately.
