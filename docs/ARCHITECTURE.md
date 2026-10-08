# Architecture and quality boundaries

## Data flow

```text
Windows apps -> group WaveRT endpoint -> Windows shared-mode loopback
  -> QPC-indexed source clock + bounded timeline
  -> per-output clock fit / delay / fractional resampling / gain
  -> WASAPI shared render -> physical device
```

Lite substitutes the current Windows multimedia default render endpoint for the group WaveRT source and stores configuration in the current user's LocalAppData. It drops that source endpoint from the mirror targets because Windows is already rendering the original mix there. Lite never registers a device, service, driver, or machine-wide configuration. The direct source cannot be delayed or clock-corrected by Lite, so its acoustic timing against mirrored outputs is not covered by the driver's synchronization/calibration claim. Per-application routes sent to a non-default endpoint are outside Lite's capture source.

The Pro driver exposes one render endpoint per software devnode, with a stable group GUID and container GUID. Its 48kHz PCM16/24/32 formats cover stereo, side 5.1 and standard 7.1. It has no offload, hardware loopback, fake hardware volume, microphone, or Bluetooth sideband pins. Windows therefore supplies software loopback and volume. Actual sample delivery and volume semantics still require installed-driver acceptance testing.

SysVAD retains ownership of WaveRT scheduling, notification and PnP contracts. OmniSound replaces its endpoint tables, disables PCM recording permanently, removes its single-adapter restriction, and guards unused sideband code. DSP, device discovery and recovery live in user mode. Upstream origins and local modifications are listed in `THIRD_PARTY_NOTICES.md`.

## Processes and contracts

- **UI:** unelevated native C++/WinUI 3 application. `IpcController` owns service/engine requests on one worker queue; callbacks marshal snapshots to the XAML dispatcher. Device enumeration runs off the UI thread and is cached until a device event or a 30-second refresh. Closing hides the window to its notification icon; explicit Exit shuts down both UI and engine. The notification icon restores the window, selects a Lite group, changes login startup, and opens Windows sound settings. List selections retain opaque endpoint IDs internally and use readable labels externally.
- **Engine:** one process per user session; only the active console session routes audio. Pro groups have independent event-driven audio threads owning capture, output streams and preallocated DSP buffers. Lite runs only the persisted active group's router; switching closes the old router before creating the new one. Pro controls publish atomic delay/routing/volume snapshots; names and those controls do not restart streams. Membership, output-processing and source-format changes reopen affected groups only. Output reconnection is bounded and independent of remaining outputs.
- **Lite controller/engine:** the engine owns versioned configuration under the logged-in user's LocalAppData, with load/save/activate commands over a separate per-session pipe/mutex. The UI uses authoritative engine readback rather than a separate file view. The engine reads WASAPI loopback from the current default multimedia render endpoint and routes only to the active group's other physical members. It runs in the user session and does not depend on the management service.
- **Service:** LocalSystem for software-device provisioning and service-owned configuration only. It never captures or renders PCM. Configurations are partitioned by the authenticated caller SID. A request may not claim another owner's group ID. Device discovery and provisioning reconcile persisted desired state after restart.
- **IPC:** local byte pipes with versioned, length-limited frames, identity/session checks, bounded I/O deadlines, stop events, and explicit response acknowledgement. There is no TCP listener or cloud dependency. Engine pipes require the same SID and session; service mutations require the active console session.

Control API version 1:

| Destination | `op` | Payload/result |
| --- | --- | --- |
| Service | `load` | Current owner's `config`, provisioning state |
| Service | `save` | Full validated config with expected revision; returns new revision |
| Engine | `status` | Per-group/member state, estimated correction, last measurement, operation progress |
| Lite engine | `load` / `save` | Authoritative config / validated config with expected revision |
| Lite engine | `activate` | Saved `groupId`; atomically persists the active selection |
| Engine | `startup` | `enabled`; saves per-user login preference and UI launch registration, then reads both back |
| Engine | `test` | `endpointId` of a physical output |
| Engine | `calibrate` | `groupId`, wired `microphoneId` |
| Engine | `cancel` | Cancels measurement/test without saving partial results |
| Engine | `shutdown` | Stops this user's engine |

The UI polls status every two seconds off its UI thread and every fifteen seconds when hidden; this provides status updates without a second persistent pipe. A successful save adopts the engine/service's committed revision and clears the dirty state. Failed commands preserve connection state and surface the IPC error. Group configuration uses schema version 1 and compare-and-swap revision numbers. Writes use flushed temporary files and an atomic replace. Unknown schema versions, duplicate identifiers, non-finite gains/delays, excess counts, unknown active groups and invalid measurement assertions are rejected.

The unpackaged WinUI app explicitly initializes the pinned Windows App SDK bootstrapper before creating XAML. The installer deploys the Microsoft-signed x64 Windows App Runtime prerequisite, then installs the signed OmniSound components; the runtime bootstrap DLL remains beside the UI executable. Release packaging verifies Microsoft's signatures separately from the OmniSound publisher signature.

## Synchronization

Source samples are tagged with absolute stream frame positions. Missing packets, ring wrap, stream reset and silence cannot expose stale PCM. Source/output clocks are fitted against QPC using bounded observation windows; nonmonotonic positions, excessive rate deviations and timestamp jumps reset the estimate. Hardware clock units are converted using `IAudioClock::GetFrequency`.

The desired source frame for an output frame is derived from its predicted presentation time, acoustic calibration offset, common group delay and member delay. A shared immutable 128-tap windowed-sinc table interpolates adjacent phases with double accumulation. Clock-rate changes use a five-second filter; phase corrections use a two-second filter. Large errors and missing packets use short fade-out/fade-in transitions. Float headroom survives until Windows applies the destination master volume; nonfinite values are silenced. No allocations, file access or blocking locks occur in the normal sample filling path. Reconnection activates a failed endpoint on a separate worker so other outputs can continue; creating this recovery worker may allocate. Rejoined devices can increase the common delay to accommodate a slower output.

Lite anchors to a complete captured block with one observed capture-packet reserve and a small interpolation margin, then follows a continuous QPC timeline instead of repeatedly clamping its playhead to packet arrivals. Real active-source starvation can grow the reserve up to 20 ms; this trades a small amount of latency for continuity. Idle capture clears old audio and fades to silence. Valid packet timestamps permit nominal-rate startup before the eight-observation drift estimate converges, once enough samples cover the interpolation guards. Render starvation detection handles both advancing and stalled hardware position counters: queue/write-time evidence resets the presentation anchor, and the submitted frame count is rebased if the hardware position passed it. Diagnostics expose active missing frames, resyncs, render starvation, float headroom and invalid samples separately.

Mirror streams request RAW mode to avoid applying optional effects a second time to an already-processed source. Both period and RAW negotiation have fresh-client compatibility fallbacks; always-on endpoint/hardware effects can remain. The existing Windows master and application-volume contract still applies.

Lite uses the minimum supported shared output period through `IAudioClient3`, with default-period fallback, and adds no fixed synchronization safety delay. Its software source master attenuation is compensated using channel dB values before the destination Windows volume is applied once. Hardware source master volume is after loopback capture and is not compensated. Application volume and source mute remain effective. Source volume notifications wake the control loop; matching a destination's percent changes its Windows endpoint master volume and therefore affects other apps using that device.

Pro volume-follow compensates only the virtual source's shared master attenuation.
Front-channel balance is not used to scale a matrix containing center/side/rear input,
so Windows per-channel balance remains in the captured PCM. Members with independent
volume retain the group master in their input; follow-volume members apply the selected
group's percentage at the physical endpoint once.

Stored acoustic offsets remain useful estimates after an engine restart, but the UI conservatively requests recalibration: device changes while the engine was stopped cannot be observed. Only a successful measurement made during the current run, with unchanged settings and device conditions, may show a currently valid calibration. Device notifications, reconnects and session switches invalidate that claim; the last measured result and its timestamp remain historical records. Manual delay adjustments do not rewrite measurement history.

Automatic measurement uses quiet, tapered deterministic noise bursts, normalized FFT correlation, ambiguity/clipping/dropout rejection, repeated trials and corrected-timing verification. Acoustic measurements include the microphone's common capture delay, which cancels in relative alignment. They apply to that microphone position. The reported uncertainty is an estimator, not a calibrated metrology certificate; independent multichannel measurement is required for release claims.

## Practical quality assessment

The device picker separates a selectable ListView row from its membership checkbox. The name/metadata area selects the device's controls without changing membership; only the compact checkbox toggles routing. In Lite, the current Windows default is sorted first, visibly labelled and cannot be checked as an additional mirror. A stored member that becomes the Lite source is excluded without erasing its saved settings; a later default change can make it usable again. Pro permits all physical outputs because its source is a separate virtual group. Names wrap within the available width. Windows theme resources and system colour notifications drive title/tray appearance.

The bulk volume checkbox operates on the selected group's additional members through the existing per-member `followReferenceVolume` contract. It shows mixed policy explicitly, preserves saved manual percentages, excludes the directly playing source and gives newly checked members the active all-follow policy. It does not introduce another persisted volume multiplier or change the source's master volume. These UI responsibilities remain separate from the engine's volume compensation and audio thread.

| Area | Design tradeoff and required evidence |
| --- | --- |
| Functionality | OS-visible endpoints require the signed kernel package; app build alone cannot establish functionality. |
| Reliability | Stable identities, atomic per-user Lite settings, revision checks, independent reconnect and bounded buffers. Exercise removal, suspend and engine crashes on hardware. |
| Performance | Event-driven processing and precomputed DSP trade a bounded amount of memory for predictable callback work. Added buffering prioritizes relative sync over video/game latency. |
| Maintainability | Core DSP/config tests run without drivers; kernel adaptation is isolated and attributed. |
| Compatibility | WASAPI shared-mode format conversion; no interception of third-party device drivers. Test real Bluetooth mode changes and Windows volume/mute. |
| Security | Lite removes the kernel/service attack surface and limits settings to the current user. Driver keeps authenticated local IPC, protected machine configuration, no persisted PCM, signed-release gates, and no boot-policy bypass. |
| Usability | Native keyboard-accessible controls, immediate work status, identification tones and separate measured/estimated states. |
| Portability | Native x64 Windows scope; static MSVC runtime. DSP/model are platform-independent, Windows interfaces remain isolated. |
| Quality in use | Driver success is selecting a Windows group and hearing aligned outputs. Lite success is mirroring the default mix to additional devices; its direct primary output cannot be held for synchronization. Explicit uncertainty, low-level test sounds and failure states reduce operational and hearing risk; failure recovery and compact controls limit user effort. |

The installer retains user configuration on removal and preserves previous application binaries during updates. A failed driver-stage operation leaves the existing app intact. A driver update already accepted by Windows may require manual rollback even if later application installation fails; the installer does not silently remove another driver package.

The C++ installer hosts provisioning commands through the system Windows PowerShell runtime. It restricts module discovery to the protected system module directory and does not modify execution policy. Application files must have valid signatures from the same publisher, and the driver catalog must carry a Microsoft Windows signature before staging. Release packaging independently applies kernel-signing-policy verification.

The normal entry point is an Inno Setup 7.1.0 single-file EXE; the C++ setup program
is its silent provisioning/rollback helper. Lite is per-user and Pro relaunches setup
for machine scope/elevation. The standard uninstaller invokes backend removal only
after the user confirms removal. Setup validates paths before provisioning, preserves
an existing UI language, records logs and rejects unsigned Pro. Release setup signs
the generated uninstaller as well as the final EXE. Development builds remain labelled
and cannot become releases using a local/self-signed test certificate.

Pro's canonical channel layout is FL/FR/FC/LFE/BL/BR/SL/SR. Its routing module maps
explicit masks, preserves stereo unity and normalizes downmix rows for headroom.
Per-output matrices are calculated on the control thread and published atomically;
the audio thread takes at most two snapshot attempts, then retains the old matrix.
Changes fade over roughly 5ms. A five-second timeline per physical Pro output costs
about 3.8MB; it keeps callback work bounded at the expense of memory. No HRTF/object
decoder is implied by PCM positioning. Source format changes reopen the group rather
than reinterpreting the old stream with a new channel count.
