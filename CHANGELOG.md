# Changelog

## 3.1.3

- Completely overhauled the Configuration Editor layout across all filter components, cards, and toolbars:
  - Eliminated redundant vertical whitespace, dead void rows, oversized paddings, and detached title bars.
  - Converted Preamp, Delay, and Channel modules into clean, compact single-row horizontal layouts, eliminating the empty void in Channel.
  - Streamlined Device and Convolution modules by consolidating headers and controls into a 3-row layout and eliminating unused rows.
  - Removed dead row 2 in BiQuad filters by moving the Reset button into row 0 alongside the filter type dropdown.
  - Corrected GraphicEQ's expanding vertical label size policy and tightened band selector spacing.
  - Tightened margins and paddings across Stage, Include, VSTPlugin, ParametricEQ, and Headphone Calibration components.
- Fixed the header Double precision switch reading as an "off" choice when the configuration needs text editing; it now shows the same "see configuration" wording as the Settings menu action.
- Isolated formula loudness crossover handoff per endpoint to protect against torn writes and cross-stream state corruption.

## 3.1.2

- Redesigned the Qt workspace with a Hibiki studio masthead, rounded signal modules, layered light/dark surfaces, engraved rotary controls and softly filled response plots. Added finite header, module, menu, profile, button, focus, status and dial animations without delaying input or audio processing; high contrast, Windows reduced motion, snapshots and `EQAPO_DISABLE_ANIMATIONS` disable motion. Existing profiles, A/B recovery and audio routing remain compatible.
- Tightened the Configuration Editor layout by removing unused space from filter rows, loudness-correction controls, and the analysis dock while retaining responsive resizing and docking behavior.
- Added a visible top-header **Double precision** switch. It continues to serialize `ProcessingPrecision: 32` when off and `ProcessingPrecision: 64` when on; configurations whose effective value depends on an `Include` or scoped `Device`/`If`/`Stage` context disable the switch and require text editing.
- Added a visible x64-only top-header **ASIO device** selector. It reuses the shared safe, non-loading discovery path to list validated x64 vendor drivers, writes only the current-user `HKCU\Software\EqualizerAPO\ASIOProxy\TargetCLSID` override, and never changes vendor registry keys, CLSIDs, or files. A selection takes effect only after the DAW reopens its audio device or the host is restarted; running streams are not hot-switched.
- Fixed the header Double precision switch going stale after hand-editing precision or scope lines; it now refreshes with every model change.
- Further tightened every filter component and both loudness dialogs by trimming paddings and spacings without changing behavior; ParametricEQ and headphone-calibration scroll viewports also start shorter.

## 3.1.1

- Fixed formula loudness contours using endpoint dB instead of the selected Linear/Squared follow attenuation; initialization, scalar-only updates and Studio previews now agree with calibration. Added explicit APO target-dB/source readouts, unambiguous amplitude-curve labels, manual dB guidance and guarded Single-endpoint rebinding when leaving manual mode. Windows volume remains read-only; automatic full-volume takeover is not implemented.
- Fixed Windows clean-checkout CI failures by pinning embedded PowerShell helper line endings and creating the ASIO SDK include directory in the isolated MSBuild argument test.

## 3.1.0

- Added an experimental x64 `Hibiki EQAPO` ASIO proxy driver for transparent DAW monitoring correction. Setup selects and records an existing x64 hardware driver once, while each DAW only needs to select Hibiki EQAPO as its global audio device; projects do not require a master-bus plug-in. The proxy keeps vendor registry/classes untouched, forwards IASIO lifecycle and callbacks, exposes stable proxy-owned DAW output buffers, and commits each completed mono/stereo PCM `FilterEngine` result to vendor buffers on the following callback. This staging reports one added ASIO block, keeps worker processing away from hardware/DMA buffers, treats host `outputReady` only as a completion signal, preserves dry audio when DSP is unavailable, and silences a missed block only while vendor-buffer write ownership remains provable; otherwise it leaves the ambiguous buffer untouched and hardware output is vendor-defined. It rejects DSD and ambiguous multichannel layouts. Registry publication, rollback, uninstall ownership, PE/import/export validation, and fake-vendor staging/order tests are covered separately; real hardware-driver/DAW validation remains a release blocker under ADR-0007, and the Monitor VST3 is retained only as an advanced fallback.
- Fixed two NSIS runtime failures found during the first real x64 proxy deployment: all five ASIO registry-enumeration loops now stop on an empty `EnumRegKey` result even when NSIS does not retain its error flag, and the embedded PowerShell process stopper no longer breaks an upgrade install by nesting `-eq '1'` inside its single-quoted NSIS parameter. A deterministic two-driver registry harness now exercises production discovery and selection through a process-local HKLM override, while a second harness extracts and runs the production command across the NSIS → nsExec → PowerShell quote boundary; focused ASIO and general installer contracts cover the surrounding transaction rules. The unsigned experimental driver was installed against a Universal Audio Volt as a registration and service smoke test only; actual DAW playback compatibility remains unverified.
- Renamed the compatibility fork's user-facing product identity to **Hibiki EQAPO**, including application titles, version metadata, update and release surfaces, documentation, installer artifacts, and the unpublished Monitor VST3 bundle and device identity, while retaining the core Equalizer APO binary names, configuration syntax, registry/install paths, and other required compatibility identifiers.
- Hardened the Voicemeeter output-insert fallback so initial callback startup failures are reported, stream changes perform a checked main-thread stop/start sequence, teardown stops and unregisters the callback before logout, and process inspection closes handles and safely handles empty command lines.
- Hardened malformed DSP, VST, updater, and installer boundaries: invalid GraphicEQ/IR data now stays dry, formula loudness rejects non-finite required levels while preserving legacy optional-field fallbacks, Benchmark rejects zero batches and non-finite output, legacy out-of-process VST rows round-trip without parameter drift, hidden MIDI targets are rejected at runtime, GUI-host shutdown preserves final state and verifies process identity before forced termination, and installer cleanup/download failures no longer silently escape their transaction boundaries.
- Made editor row removal a two-phase, vetoable transaction: canceling a close or drag no longer stops an out-of-process VST host, recovered state is captured before the row changes, and failed `.opvs` or `.pid` cleanup remains visible and retryable.
- Hardened installer recovery and uninstall with restartable journals, path-bound embedded process shutdown, root-only configuration ACL updates, and reparse-aware directory identity checks; existing user-writable configuration descendants are never traversed or written by the elevated installer.
- Made the Windows build and release inputs hermetic and reproducible by isolating MSBuild imports and environment properties, pinning Python and hashed extraction wheels, and verifying exact Qt archive sizes and SHA-256 hashes before transactional extraction.
- Added an x64 `HibikiEQAPOMonitor.vst3` wrapper for DAW Monitor FX/Control Room/Listen Bus use while retaining the vendor ASIO driver, with native float/double processing, offline dry bypass, stable monitor-device scoping, Windows-compatible speaker-layout validation, complete local build/staging wiring, and a hash-manifested standalone install/update/uninstall manager with fixed crash-recovery slots. Its dedicated `x64-windows-static-md` graph embeds FFTW, libsndfile, and codecs to prevent generic DLL basename collisions inside a DAW; Release staging now audits both normal and delay-load PE imports before accepting the self-contained module plus app-local VC runtime. Shared VST library caching, VST3 module entry/factory teardown, VST2 time state, editor class registration, and logging state are serialized for parallel DAW instances. Both `VSTPlugin:` and `OutProcVSTPlugin:` reject recursive Monitor self-loading by physical file identity before a plug-in load or child host can begin. The main NSIS and release workflow do not deploy it automatically, and real-time export, third-party static-link release obligations, plus real-DAW validation limits remain explicit.
- Shortened the Convolution editor entry to **IR convolution** and hardened IR loading/rebuilding against invalid shapes, partial reads, non-finite samples, path collisions, incomplete output replacement, repeated FFT planning, and conversion while typing. Stable callback sizes below the initialization maximum now receive a background-built bank and 10 ms dry-to-wet fade instead of remaining bypassed; each channel is capped at 4,096 HybridConv partitions to reject pathological long-IR/small-block allocation storms; sample-rate matching is an explicit minimum-phase magnitude reconstruction so phase-sensitive IRs are not silently transformed.
- Added a completely separate `LoudnessCorrectionOriginal:` component that preserves Mixomo's original 75 Hz/10 kHz two-shelf model and default-Multimedia-endpoint tracking without sharing the formula component's command, state, calibration, engine, or volume-follow controls.
- Fixed the original shelf calculation's uninitialized neutral branch so enabling it at the neutral point no longer starts with an unintended level reduction, and added stable low-sample-rate handling plus lock-free smoothed coefficient publication.
- Added VST2/VST3 MIDI learn for CC, Note, and Pitch Bend hardware controls, using stable automatable VST3 parameter IDs, guarded VST2 indices, bounded real-time queues, reconnectable device identity, and backward-compatible out-of-process parameter metadata. Learning now uses the recoverable temporary-audio transaction to release an existing mapping from the current row before opening a single-client WinMM device, then restores it before applying the result.
- Fixed invalid zero-based Qt placeholders that left version numbers, analysis values, profile status text, serialized numeric settings, exported FIR names, and VST permission warnings showing literal `%0` text instead of their values.
- Stopped calibration pink noise before either loudness dialog reports acceptance or cancellation, so temporary-audio restoration prompts cannot leave the signal playing behind a closed dialog; choosing to keep an externally edited configuration now also discards the pending measurement instead of overwriting that file.
- Reduced the default Full loudness engine's settled callback cost with a preallocated section-major block path while retaining the transition fallback and sample-for-sample native equivalence coverage.
- Reduced Full-engine initialization and volume-update cost by reusing each frequency's complex terms and aggregating the correction cascade before magnitude conversion.
- Kept the lower-cost, at-most-two-section Fast engine as an explicitly labeled experimental option because extreme low-level response tests can differ from Full by roughly 10–15 dB.
- Added optional read-only APO volume following for Matrix-style routes that report a Windows volume without applying it to audio. `Off` remains the compatible default; `Linear`, squared `Logarithmic`, and endpoint-dB-based `Windows` curves attenuate the complete post-correction output without writing system volume.
- Smoothed volume-follow changes over 10 ms, mapped endpoint mute to exact silence, kept startup muted until the first valid automatic-volume snapshot, and held the last successful attenuation across transient runtime read failures instead of jumping to full volume.
- Removed the unconditional 1 dB correction-branch headroom margin that made nearly neutral correction start quieter, while retaining peak-derived attenuation and the complete-transfer safety scan.
- Made endpoint notification callbacks own their change signal so a failed Windows callback unregister cannot leave a pointer into a destroyed controller.

## 3.0.7

- Fixed the Configuration Editor startup crash that v3.0.6 could trigger while showing a window after restoring an older Qt toolbar layout.
- Versioned the saved `QMainWindow` layout state so incompatible version-0 toolbar and analysis-panel placement is rejected without touching configuration files, audio settings, profiles, endpoint binding, or other editor preferences.
- Smoothed low-frequency loudness response by optimizing filter quality factor (Q=2.2), eliminating ripples and oscillations between 20 Hz and 100 Hz.
- Corrected high-frequency compensation above 12.5 kHz to follow the natural upward contour using a high-shelf biquad transition, preventing the abrupt cliff drop.
- Fixed excessive vertical height in the Copy Channels editor by binding both tabs to a compact row height.
- Removed the redundant contributor banner from the Loudness Correction panel header for a cleaner layout.
- Kept Reset actions compact without displacing stretch space, restored full row width for the Copy editor, and aligned Preamp and Delay component titles to the top.

## 3.0.6

- Fixed committed installer cleanup so its protected rename manifest remains open on a dedicated handle while old application-file identities are checked. This prevents the first identity check from corrupting the manifest handle or installation-path containment prefix and leaving a false interrupted-installation record.
- Added regression coverage for the handle lifetime and containment state used across multi-record cleanup. Existing committed v3.0.5 journals are resumed without rollback; ambiguous `.old` files remain untouched.
- Restored the intended two-row editor toolbar after loading older saved window states, preventing profile controls from hiding search, comparison, and bypass actions.
- Reduced dense filter-row height by removing the permanent VST compatibility notice, moving the repeated local IR/FIR notice into accessible help, showing manual FIR matching only when it is actionable, and bounding saved Copy-panel heights.
- Replaced the analysis loading bar with a compact text status while retaining numeric headroom, busy-state accessibility, and Auto preamp availability feedback.
- Includes the v3.0.5 Auto preamp command, which performs a fresh identity-bound analysis and can only reduce gain so the current estimated peak does not exceed 0 dB.

## 3.0.5

- Ported the complete Mixomo `exp` feature code onto this fork rather than using its reduced, outdated `main` tree, retaining native audio tools, Parametric EQ, user-supplied headphone calibration and convolution/IR workflows, VST3 class selection, and the experimental out-of-process VST host.
- Added a Windows-native, responsive desktop interface across Configuration Editor, Device Selector, device testing, and Update Checker, with improved high-DPI scaling, keyboard access, screen-reader labels, and translated status feedback.
- Added quick profile access, duplication, import/export, editor-only device links, temporary bypass, A/B comparison, and tray controls to Configuration Editor while retaining the v3.0.4 loudness-correction runtime and endpoint-binding behavior.
- Improved dense filter editors and plots for small windows and large text, and added automated UI layout and regression checks.
- Corrected circular-control direction and added relative vertical dragging with fine adjustment; added an explicit command that returns a floating analysis panel to the bottom dock.
- Added an accessibility-aware response-curve transition and a confirmed one-time Auto preamp action that only reduces gain from a fresh, identity-bound current-file analysis. The action validates the exact root/Include bytes and automatic-volume endpoint snapshot, refuses unsupported dynamic/cross-channel/external processing, cannot widen a scoped edit, and always requires an explicit Save.
- Hardened the restored VU Meter shared-state handoff, reset, multichannel-selector preservation, and sample-peak/ungated-estimate labeling, and improved cold-start/reload ownership for the experimental out-of-process VST host.
- Made Device Test cancellation finish any fallback registration transaction and required Windows audio-service restart already in progress, avoiding a half-applied endpoint state.
- Excluded third-party headphone-measurement catalogs and impulse-response audio from public history and installers until their exact downstream redistribution terms can be established; the corresponding loaders remain available for user-supplied data.

## 3.0.4

- Fixed Configuration Editor analysis so the first displayed response uses the saved loudness-correction settings; changing `ReferenceOffset` now changes the displayed curve immediately instead of being hidden by the realtime cold-start bypass.
- Kept unavailable automatic-volume bindings fail-closed during offline analysis, and added native and runtime regressions that prove `ReferenceOffset` changes processed output while disabled or unavailable filters remain bit-transparent.
- Corrected the Single/Global binding guidance for Matrix-style routing. Global is appropriate only when the Windows default Multimedia master volume is the intended shared control; muted or fixed virtual defaults require Single or manual volume.

## 3.0.3

- Fixed the v3.0.2 installer recovery loop caused by NSIS append mode preserving a file without moving its write pointer to the end. The next installer now retires the corrupted protected manifest, keeps ambiguous `.old` backups untouched, finishes committed recovery cleanup, and continues installation.
- Records renamed application files only after Windows confirms the rename, uses UTF-16 framed records, binds cleanup to each file's stable identity, deletes through the verified handle, and flushes a one-shot cleanup gate before touching product files.
- Added a native NSIS append regression harness to the release build in addition to the installer transaction contract tests.

## 3.0.2

- Rebuilt the fork-specific changes directly on the current `Mixomo/EqAPO64_with_VST3_support` main-line history while preserving its original Equalizer APO and VST-hosting foundation.
- Added the explicit `Schema 1 Model FormulaLoudnessV1` marker. Every unmarked loudness entry now remains unchanged and bypassed until Configuration Editor is told either to keep the previously released formula values or convert the original shelf profile.
- Made live coefficient reloads real-time safe with preallocated banks, atomic publication, background reclamation, and a 100 ms crossfade in the common crossover domain.
- Added explicit Single and Global automatic-volume bindings. Single follows the actual APO playback endpoint and fails closed for capture or unknown flows. Global preserves the original Mixomo behavior: every instance reads the current Windows default Multimedia playback volume without depending on the APO endpoint metadata, for routing systems such as VB-Audio Matrix. A missing endpoint, failed rebind, or unreadable volume source fails closed: correction fades to the uncorrected `A = L + H` path over 10 ms, then recovery silently warms for 250 ms and fades correction back in over 100 ms.
- Kept the settled A-domain magnitude from 1-19 Hz within 0.01 dB of unity with a fixed high-order crossover, without applying the correction branch's headroom reduction. Cold start remains raw for at least 1.0 s and then hands each channel to A only at a bounded sampled crossing; no timeout forces an unsafe handoff.
- Added a persistent installer recovery journal and delayed the transaction commit until application files, permissions, registry data, shortcuts, device selection, and updater configuration have completed. Interrupted upgrades are recovered on the next setup run.
- Gave the installer and updater a fork-specific user-facing identity while retaining the shared Equalizer APO paths and registry keys required for in-place compatibility.
- Added a Traditional Chinese interface and migration guidance, pinned the x64 build/release toolchain, published SHA-256 artifacts, and kept automatic update checks opt-in.
- Presented the feature only as loudness correction, without a standards-conformance, certification, endorsement, affiliation, or approval claim.

## 3.0.1

- Replaced the incorrect loudness data path with the 29-point formula parameter table and direct runtime evaluation.
- Bound automatic volume tracking to the actual playback endpoint selected by Equalizer APO. Capture, missing-endpoint, and endpoint-read failures now bypass correction safely.
- Added a 100 ms dual-bank coefficient crossfade and dense, locally refined peak analysis with a 1 dB headroom margin.
- Preserved the original command and parameter text of 2.0.0 entries until the user explicitly converts them to the enabled formula profile.
- Blocked calibration noise when the selected endpoint is not the Windows default Console playback endpoint, and clarified the one-speaker SPL measurement procedure.
- Made automatic update checks opt-in and strengthened installer cleanup and rollback behavior.
- Added native runtime regressions for difficult low- and high-frequency peak cases. Release installers remain unsigned; verify the published SHA-256 file before installation.
- Described the feature only as loudness correction; no standards-conformance claim is made.

## 3.0.0

- Restored the data-driven 29-band loudness-correction engine, reference-level controls, manual or endpoint-volume tracking, and pink-noise calibration.
- Rebranded the feature as **Loudness Correction**. The project does not claim standards conformance, certification, endorsement, affiliation, or approval.
- Kept the current GitHub update endpoint and installer checksum workflow.
- Restored the `State`, `ReferenceLevel`, `ReferenceOffset`, `Attenuation`, and optional `Volume` configuration format. Existing 2.0.0 loudness-correction entries open as disabled drafts and must be reviewed and recalibrated before enabling.
