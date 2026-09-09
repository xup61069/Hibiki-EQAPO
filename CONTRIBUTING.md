# Contributing

Thank you for improving Hibiki EQAPO, an unofficial Equalizer APO compatibility fork. Bug fixes, tested device-compatibility improvements, translations, and documentation corrections are welcome.

## Before opening a change

1. Search existing issues and pull requests.
2. Keep changes focused and preserve compatibility with existing Equalizer APO configuration files where practical.
3. Do not commit credentials, generated build directories, installers, proprietary plug-ins, or proprietary source material.
4. New acoustic data must identify its source and confirm that public redistribution is permitted.

## Local checks

Use 64-bit Windows with Git, **CPython 3.13.2 x64**, CMake, and Visual Studio's Desktop development with C++ workload. The Qt extractor dependency lock targets this exact Python ABI. Every change runs the baseline checks:

```powershell
python -m unittest discover -s .\tests -p "test_*.py" -v
git diff --check
```

Add checks according to the files and behavior changed:

| Scope | Additional check |
|---|---|
| C++, DSP, project wiring, or installer | `.\scripts\build-installer-x64.ps1 -Configuration Release` followed by `.\scripts\test-runtime-loudness.ps1 -Configuration Release` |
| Out-of-process VST host | `python -m unittest discover -s .\tests -p "test_outproc_vst_lifecycle.py" -v` |
| UI | `.\scripts\capture-ui-regression.ps1 -Configuration Release`, then inspect the generated matrix |
| UI motion | `.\scripts\test-ui-motion.ps1` checks input delivery, cancellation, bounded animation lifetime and reduced motion in a native Qt harness |
| PR or release preparation | `.\scripts\test-public-history.ps1 -Revision HEAD` |

The expanded C++/DSP sequence is:

```powershell
.\scripts\build-installer-x64.ps1 -Configuration Release
.\scripts\test-runtime-loudness.ps1 -Configuration Release
```

For DSP changes, describe the expected response, sample rates tested, numerical error, headroom behavior, and any CPU trade-off. For UI changes, check English, `zh_CN`, and `zh_TW`; do not submit unfinished Traditional Chinese strings. Changes to the loudness-profile data or implementation must also update [NOTICE.md](NOTICE.md) when needed.

## Code and commits

- Follow the surrounding C++ and PowerShell style; C++ files in this project generally use tabs.
- Keep real-time audio processing free of allocation, locks that can block, logging, and system API calls.
- Do expensive curve fitting and endpoint queries outside the audio callback.
- Add a regression test for every numerical or parsing bug that can be tested independently.
- Explain user-visible changes under `Unreleased` or the applicable release in `CHANGELOG.md`; do not duplicate release logs into README files.
- Use clear, imperative commit subjects.

## Pull requests

Include the problem, implementation, verification performed, and any remaining limitations. GitHub Actions must pass before merge. By contributing, you agree that your contribution is distributed under this repository's GPL-3.0 license and that you have the right to submit it.
