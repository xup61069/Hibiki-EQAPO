# Hibiki EQAPO 透明 ASIO Proxy 工程驗證報告

_日期：2026-09-06（Asia/Taipei）｜建置：x64 Release｜狀態：實驗性工程候選；實機安裝／註冊 smoke test 通過；ADR-0007 Proposed_

## 結論與宣稱邊界

透明 proxy 方案已完成原始碼、fake-vendor、Release binary、installer ownership 與一輪實機安裝／註冊 smoke test。使用者在安裝時選一次底層原廠 ASIO driver，之後只需在每套 DAW 的全域 Audio Device 設定選一次 **Hibiki EQAPO**；新舊專案不需要掛、移除或記住任何 master-bus VST。

目前已將 proxy 指向本機 Universal Audio Volt driver 並成功完成安裝、ASIO／COM publication 與服務重啟，但尚未啟動真實 DAW 或送出音訊。這輪 smoke test 不等於完成真實音訊介面原廠 driver＋DAW matrix，因此本報告只支持「工程上可行、自動化 gate 與安裝／註冊通過」，不支持「已相容所有 ASIO driver／DAW」或「可直接正式發布」。在播放 gate 完成前，[ADR-0007](decisions/0007-transparent-asio-proxy.md) 維持 Proposed，正式安裝包必須停用或排除 proxy。

## 使用者工作流

```text
DAW → Hibiki EQAPO → 使用者選定的原廠 ASIO driver → 音訊介面
```

1. 安裝程式列出有效的 x64 machine-wide ASIO drivers，使用者選擇底層 driver；proxy 不改寫原廠 CLSID、COM class 或 DLL。
2. DAW 第一次選擇 `Hibiki EQAPO` 後，由 DAW 保存全域偏好；專案檔不需要 VST insert。
3. Proxy 讀取既有 `config.txt`，以固定 `Device: Hibiki EQAPO` scope 執行 callback-safe 的 `FilterEngine` 設定。
4. `HibikiEQAPOMonitor.vst3` 僅是無法使用 proxy 時的進階 monitor-only 備援，主 NSIS 不安裝它。

## Buffer ownership 與固定延遲

- DAW 只看見 proxy-owned output double buffers；vendor buffers 只由序列化的 vendor callback 寫入。
- 每次 vendor callback 先提交上一個已完成的 private stage；若 deadline miss 且仍可證明目前 vendor half 可寫，提交靜音。若 ownership 不可證，proxy 完全不碰該 buffer，實際輸出由原廠 driver 決定。
- 提交後才可送出可選的 vendor `outputReady()` hint，再通知 host 填入目前 proxy half。Host 的 `outputReady()` 是 async completion，絕不 1:1 轉送給 vendor。
- `directProcess=true` 在 host callback 返回後同步完成 DSP；`directProcess=false` 以預配置 FIFO token 和 exactly-once、FIFO completion 完成 DSP。
- Output path 固定增加一個 DAW 所選的 ASIO buffer block，並由 `getLatencies`／支援的 internal-buffer query 回報。Input-only stream 不增加此延遲；`Delay:`、`Convolution:` 等 filter 自身 latency／tail 尚未額外回報。

## 支援範圍

- x64 DAW 與 x64 原廠 ASIO driver。
- 只向 DAW 宣告原廠前一或兩個 output channels，作為主監聽 mono／stereo pair；其他 output pair 與三聲道以上 layout 尚不支援。
- 支援 18 種 ASIO PCM 格式：16／24／32-bit integer、32／64-bit float，以及 16／18／20／24 valid bits in 32-bit container，均含 little-endian／big-endian；輸出飽和並處理 NaN／Infinity。
- DSD 明確拒絕，不將 DSD bits 當成 PCM EQ。
- 第一版只支援序列化、非重入的 vendor callbacks；非同步 host 每個 block 必須恰好一次、依 FIFO 呼叫 completion。
- 同一 DAW process 只允許一個 prepared proxy instance；不同 process 的同時使用取決於原廠 driver 的 multi-client 能力。

## 設定安全策略

ASIO callback 只接受可證明不配置、不等待、不做 I/O 的設定。Active `VSTPlugin:`、`OutProcVSTPlugin:`、`OutProcGain:`、`OutProcBiquad:`、`VUMeter:` 與原版響度校正會拒絕整份新設定；公式版響度校正只有明確固定 `Volume` 時可用。Inactive `Device:`／`If:` scope 與 `State 0` 可保留。

初次載入不安全或不可讀設定時保持乾聲；reload 失敗時保留上一份已完整發布的安全設定。ASIO 沒有跨廠牌的硬體音量 API，介面旋鈕、onboard mixer／DSP、hardware direct monitor 與類比增益都無法由 proxy 自動追蹤，必須使用對應實際 SPL 的手動 `Volume`。

## Target discovery 與安裝 ownership

- 公開 driver name：`Hibiki EQAPO`；固定 CLSID：`{D47C55C9-3F7D-422F-86E9-32E170815D53}`；DLL：`HibikiEQAPODriver.dll`。
- Runtime 只接受 HKLM x64 ASIO enumeration，驗證有效 HKCR view 的 `InprocServer32` 仍與 machine registration 指向同一實體 AMD64 PE32+ DLL，並拒絕自己或遞迴 target。
- Machine default 位於既有相容 registry root `Software\EqualizerAPO\ASIOProxy`；每使用者 override 優先。這個 root、核心 `EqualizerAPO.dll`、預設安裝路徑與設定語法是升級相容識別，不因品牌改名而破壞。
- Installer journal 在任何 registry mutation 前保存精確狀態；publication、rollback 與 uninstall 只處理自己的 fixed registration，不修改 vendor key。
- Silent install 只有明確 `/ASIOCLSID=...`、既有有效 default、唯一候選或 `/NOASIOPROXY` 才能決定；多候選歧義不猜測，也不留下半註冊 driver。

## 實機安裝發現與修正

首次在已有舊版相容安裝的機器上部署時，實機流程抓到兩個只在 NSIS runtime／升級路徑出現的問題：

1. `EnumRegKey` 到達列舉尾端時會回傳空字串，但不保證保留 `${Errors}`。原本只看 error flag 的迴圈因而可能無限增加 index，安裝程式在尚未寫入 registry 或檔案前持續佔用 CPU。五個 ASIO registry 列舉點現在都先清空輸出，並以「`${Errors}` 或空字串」作為終止條件；deterministic 64-bit harness 以 process-local HKLM override 建立兩個隔離 driver，實跑 production discovery／selection 的歧義、有效指定與遺失指定路徑。16 項 ASIO installer contract 另驗證五處完整 EOF 終止序列。
2. `RunEmbeddedProcessStopper` 將 PowerShell command 放在 NSIS 單引號參數內，command 中原有的 `-eq '1'` 又提早結束該參數，導致已有安裝版本的 process-stopper 升級路徑解析失敗。比較式已改為 `if([int]$env:EQAPO_PROCESS_PROTECT_INTERACTIVE -eq 1)`，避免內層引號；除了 40 項 installer contract，另有 2 項 runtime gate 從 production macro 抽取實際命令，經 NSIS → nsExec → PowerShell 驗證開／關兩種參數。

修正後的 unsigned experimental x64 installer 已成功完成實機安裝，底層 target 固定為 `Universal Audio Volt`，CLSID `{7FA0A3EC-EBB7-4249-9CDC-F5474EE19B74}`。已驗證安裝檔案雜湊、`Hibiki EQAPO` ASIO publication、proxy COM `InprocServer32`、target registry、Windows Audio／Audio Endpoint Builder 服務，以及安裝完成後不存在殘留 recovery journal；尚未進行 DAW playback。

## 最終本機驗證

| Gate | 結果 |
| --- | --- |
| Release build／installer | Full native、Qt 與 staging gates 通過，native projects 0 warnings／0 errors；最後一次 orchestration 在舊 installer process 鎖住輸出時停於 NSIS，清除那些精確 PID 後 production `makensis` rerun 成功。新增 test-only gates 後未再覆寫 final installer |
| ASIO native core | PASS |
| ASIO fake-vendor driver | 41 個 named main tests 與 isolated-process modes 全部 PASS |
| Full Python contracts | 387 run；386 pass；0 fail／error；1 expected skip（無法建立 `Global\` mapping，Win32 error 5） |
| ASIO installer deterministic discovery | 2／2 PASS；隔離的兩-driver 64-bit fixture 覆蓋 silent ambiguity、有效 `/ASIOCLSID`、遺失 target 與零 HKCU 殘留 |
| ASIO installer contracts | 16／16 PASS；覆蓋五個 `EnumRegKey` 的 output 清空、`ClearErrors`、empty／error guard 與真正離開迴圈 |
| Process-stopper NSIS quote boundary | 2／2 PASS；抽取 production command，經 NSIS → nsExec → PowerShell 驗證 protectInteractive 關／開 |
| Installer contracts | 40／40 PASS；覆蓋其餘 installer transaction／recovery 契約 |
| 實機安裝／註冊 | PASS；TargetCLSID 為 Volt `{7FA0A3EC-EBB7-4249-9CDC-F5474EE19B74}`；ASIO／COM／服務／recovery journal 驗證通過 |
| Runtime DSP | HybridConv、數值防護、FilterEngine handoff／reload、響度 runtime 全部 PASS |
| ASIO PE gate | AMD64 PE；4 exports；22 normal imports；0 delay imports |
| Monitor fallback PE gate | AMD64 PE；3 exports；22 normal imports；0 delay imports |
| Monitor self-load guard | direct path PASS；hardlink PASS |
| 官方 VST3 validator | 537 passed；0 failed |
| Branding contracts | Runtime／UI／HTTP／installer 使用 Hibiki EQAPO；必要 Equalizer APO 相容識別保持穩定 |

驗證環境為 Visual Studio 2026 18.9.1、MSVC 14.51.36231、Windows SDK 10.0.26100.0。以上皆為目前授權本機工作樹結果；本輪已安裝 experimental driver、寫入其 owned registry 並重啟音訊服務，但沒有啟動 DAW、進行 playback、commit、push、tag 或發布 release。

## Final artifacts

| 產物 | Bytes | SHA-256 | 簽章 |
| --- | ---: | --- | --- |
| `Setup/Hibiki-EQAPO-x64-3.0.7.exe` | 16,105,896 | `3AE378D8995C8533A10FF07AFE6C756361C5013D39FE52707B993693416BB594` | NotSigned |
| `x64/Release/HibikiEQAPODriver.dll` | 5,179,392 | `6DD6672B390966728A547F0F352F5F116912F45B1049DEB10F739881A9E4C79F` | NotSigned |
| `build/VST3/Release/HibikiEQAPO/HibikiEQAPOMonitor.vst3/Contents/x86_64-win/HibikiEQAPOMonitor.vst3` | 5,146,112 | `492676b1fc914f69ea73c660566942b6880b71085ede4ad73598d5675f1a4d95` | NotSigned |

Driver source 與 `Setup/lib64/HibikiEQAPODriver.dll` 的 length／SHA-256 完全一致；Monitor source 與 `Setup/lib64/VST3/HibikiEQAPOMonitor.vst3` 的五檔 payload 亦由 staging gate 比對一致。

## 尚未驗證與 release blocker

至少需要以一組真實 x64 vendor driver＋真實 DAW，在低音量、可復原環境完成並記錄：

- 播放、start／stop、失敗後 reopen 與長時穩定性；
- 取樣率切換／reset、所有宣稱支援的 buffer size、不同 PCM formats；
- offline bounce、real-time export、record／overdub alignment；
- 裝置拔除／重接、DAW／driver restart、multi-client 行為；
- COM activation apartment、原廠 control panel 與實際 latency 報告。

其他已知限制包括固定 +1 ASIO block、filter latency／tail 不完整、只支援第一個 mono／stereo output pair、DSD／multichannel 不支援、callback／completion 契約嚴格，以及 failure 時在 ownership 不可證的情況下無法保證硬體靜音。Installer、driver 與 fallback VST3 目前都未簽章。

## 重現命令

```powershell
.\scripts\build-installer-x64.ps1 -Configuration Release
python -B -m unittest discover -s tests -p "test_*.py" -v
python -B .\tests\test_asio_installer_discovery_runtime.py -v
python -B .\tests\test_installer_process_stopper_nsis_runtime.py -v
python -B .\tests\test_asio_installer_contract.py -v
python -B .\tests\test_installer_contract.py -v
.\scripts\test-runtime-loudness.ps1 -Configuration Release
.\scripts\test-asio-proxy-binary.ps1 -Path .\x64\Release\HibikiEQAPODriver.dll
.\scripts\test-monitor-vst3-binary.ps1 -Configuration Release
.\scripts\test-vst-self-load-guard.ps1 -Configuration Release
.\scripts\test-monitor-vst3-validator.ps1 -Configuration Release
git diff --check
```
