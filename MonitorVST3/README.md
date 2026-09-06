# Hibiki EQAPO Monitor VST3 (advanced fallback)

This advanced x64 VST3 fallback lets a DAW keep using its vendor ASIO driver while
the existing Hibiki EQAPO `FilterEngine` processes the DAW's final monitor
signal. It is not an ASIO driver or bridge, does not replace the vendor driver,
and cannot process an interface's hardware direct-monitoring, onboard mixer/DSP,
or analog output path. It reads the registered Hibiki EQAPO configuration
instead of storing a second EQ configuration in the DAW session.

The unpublished wrapper uses the Hibiki identity consistently: its technical
bundle is `HibikiEQAPOMonitor.vst3`, while the existing VST3 FUIDs remain fixed
so builds of this repository keep the same component identity.

## Build, stage, install, and remove

After the pinned third-party dependencies are prepared, the normal local x64 build creates:

`build\VST3\Release\HibikiEQAPO\HibikiEQAPOMonitor.vst3`

```powershell
.\scripts\bootstrap-third-party.ps1
.\build-local-x64.ps1 -Configuration Release
```

`stage-installer-x64.ps1` first audits both normal and delay-load PE imports,
then validates the statically linked module and copies it with its four app-local
Visual C++ runtime files into the installer staging tree for integrity checks.
The audit fails if FFTW, libsndfile, or a codec is still a dynamic dependency.
The main NSIS script does not consume that VST3 staging subtree, and this step
does not change the system VST3 directory:

```powershell
.\scripts\stage-installer-x64.ps1 -Configuration Release
```

Installation is an explicit, separate action. Close every DAW first and run an
elevated PowerShell:

```powershell
.\scripts\manage-monitor-vst3.ps1 -Action Install -Configuration Release
```

The default destination is
`%CommonProgramFiles%\VST3\HibikiEQAPO\HibikiEQAPOMonitor.vst3`. The manager
accepts only the fixed bundle shape, verifies every payload as an AMD64 PE32+
image, and audits the module's normal and delay import tables before copying it.
This read-only audit uses the shared PowerShell parser and does not require
Visual Studio at install time. The manager records the length and SHA-256 of
every owned file, and refuses to update or remove a reparse point, altered
bundle, or directory containing anything it does not own. It must run in 64-bit
PowerShell and installs only Release bundles. Updates use fixed `.new`, `.backup`,
`.failed`, and `.removed` siblings so a later invocation can finish or roll back
an operation interrupted between atomic directory renames. Remove it with:

```powershell
.\scripts\manage-monitor-vst3.ps1 -Action Uninstall
```

The main NSIS installer does not install, update, repair, or uninstall this
external bundle. Even a local build and VST 3 validator pass are engineering
evidence, not a claim that the plug-in has completed the real-DAW release
matrix.

## DAW routing and export boundary

Keep the DAW on its vendor ASIO driver. Place the plug-in only as the last
correction stage in a **Monitor FX**, **Control Room**, or **Listen Bus** slot
that the DAW documents as excluded from export, bounce, and freeze. Do not put
it on an ordinary track, renderable bus, or master insert. Whenever the host
explicitly declares offline processing during `setupProcessing()`, the processor
releases its correction engine, skips configuration initialization, and sends
dry audio. If only an individual process block is marked offline, that block is
copied dry without touching the already-created engine or watcher on the audio
thread.

Export safety limit: the plug-in cannot identify every real-time export and cannot prevent a user from placing it on a renderable master bus.

Real-time export can still include the correction when the host reports the
operation as real time. A normal master-bus insert is also renderable. The DAW
routing remains the primary export boundary; offline dry bypass is only a
defensive fallback.

## Configuration and device scope

The wrapper presents this stable synthetic device string to `FilterEngine`:

`DAW Monitor Insert Hibiki EQAPO Monitor VST3`

A monitor-only section of the shared Hibiki EQAPO configuration can therefore
be scoped as follows:

```text
Device: DAW Monitor Insert Hibiki EQAPO Monitor VST3
LoudnessCorrection: Schema 1 Model FormulaLoudnessV1 Binding Single State 1 ReferenceLevel 80 ReferenceOffset 0 Attenuation 1.0 Volume -38.0
Device: all
```

Change the example `Volume` to the actual listening level and keep it updated
when the interface's hardware knob or analog monitor gain changes. Automatic
Windows endpoint-volume tracking cannot represent hardware ASIO or analog
monitor volume. `Device: all` restores the scope for following commands; omit
it only when the remainder of that file is intentionally monitor-only. Any
unscoped command in the shared configuration also runs in this wrapper. A
shared `VSTPlugin:` or `OutProcVSTPlugin:` command that resolves back to this
same Monitor module is rejected before in-process loading or out-of-process host
creation. The guard compares physical file identity, including filesystem path
aliases; keep the module out of the monitor scope rather than relying on that
dry, diagnostic fallback.

## Runtime limits

The plug-in exposes one automatable Bypass parameter. Bypass changes take effect
at the next process-block boundary; they are not sample-accurate automation.
It accepts matching main
input/output layouts made only from the 18 Windows speaker positions (plus VST3
mono), and processes both native 32-bit and 64-bit sample buffers. It is an x64
plug-in and cannot be loaded by a 32-bit DAW. Those VST3 positions map directly
to Hibiki EQAPO channel names. Wide, Ambisonics, and other positions outside
the Windows speaker mask are rejected instead of risking a wrong per-channel
correction.

End-to-end real-time behavior still depends on the selected Hibiki EQAPO
filters. Configurations containing out-of-process processing or third-party
VST plug-ins can wait on external code and require separate deadline testing.
If the registered `ConfigPath` or its `config.txt` cannot be read, the wrapper
does not treat buffer allocation alone as a ready engine and remains dry. The
wrapper also cannot derive and report dynamic VST3 latency or tail metadata for
arbitrary `Delay:`, `Convolution:`, `VSTPlugin:`, or out-of-process commands;
keep it on a monitor-only path where that limitation is acceptable.
FFTW, libsndfile, and libsndfile's codec graph are statically linked from the
dedicated `x64-windows-static-md` vcpkg tree. This avoids exposing generic codec
DLL basenames inside a DAW process where another plug-in may already have loaded
an ABI-incompatible build. The Release bundle carries only the module and the
four supported Microsoft Visual C++ runtime DLLs. The processor is intentionally
not marked VST3 distributable because it reads the local Hibiki EQAPO compatibility registry
and configuration; a host must not move it to a remote processing node. The
build/staging payload is those five files; an installed bundle also contains the
manager-owned `Contents\Resources\hibiki-eqapo-monitor.json` manifest used to
verify safe updates and removal.
