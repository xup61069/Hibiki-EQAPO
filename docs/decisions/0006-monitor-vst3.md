# ADR-0006：Vendor ASIO 的 monitor-only VST3

## 狀態

Accepted

## 日期

2026-09-06

## 背景

目標使用情境是讓 DAW 繼續直接使用音訊介面的 vendor ASIO driver，並只在實際監聽輸出套用 Hibiki EQAPO 的 `FilterEngine`，供喇叭或耳機校正使用。正常匯出不應把監聽校正燒進成品。

VST3 沒有跨 DAW 通用的「這一定是 monitor slot」註冊類別，也無法可靠分辨所有即時播放與 real-time export。許多 DAW 提供不參與匯出的 Control Room、Monitor FX 或 Listen Bus；這才是主要的匯出隔離邊界。Host 宣告 `ProcessModes::kOffline` 時可再做防禦性乾聲旁路，但不能涵蓋以 `kRealtime` 執行的匯出，也不能阻止使用者把外掛誤放在可渲染的 master bus。

## 決策

### 產品與匯出邊界

新增獨立的 `HibikiEQAPOMonitor.vst3` audio-effect bundle。它只包裝既有 `FilterEngine`，不攔截 ASIO、不取代 vendor driver，也不修改 Windows APO 註冊。

正常使用時，使用者必須把它放在 DAW 明確標示為不參與 export/bounce/freeze 的 monitor、control-room 或 listen-bus FX slot。若 DAW 沒有這種 slot，此功能不提供可證明的「只監聽」保證；應改用 Voicemeeter output insert、外部 DSP 或其他可回溯的監聽路由。

Processor 在 `setupProcessing()` 記住 host 的 process mode；進入 `ProcessModes::kOffline` 時會釋放既有 engine，且不載入 DSP runtime、不解析設定或啟動設定 watcher。`process()` 也逐 block 檢查 host mode，offline 一律輸出逐樣本乾聲且不呼叫 `FilterEngine`，作為常見 offline export 的第二層保護。公開文件仍必須包含下列完整警語，不得改寫成自動保證：

> Export safety limit: the plug-in cannot identify every real-time export and cannot prevent a user from placing it on a renderable master bus.

文件不得宣稱外掛會自動避免誤放 master、永遠不會被 render，或放在任何 insert 都安全。real-time export、外部硬體回錄與 host 未正確回報 offline mode 時，是否燒入完全取決於 DAW routing。

### VST3 factory 與穩定識別

Factory 使用 `BEGIN_FACTORY_DEF`／`DEF_CLASS2` 註冊 processor 與 controller：

- processor 類別為 `kVstAudioEffectClass`；
- subcategory 必須包含 `Vst::PlugType::kFx`，不得註冊成 instrument；
- processor class flags 固定為 `0`，不得宣告 `Vst::kDistributable`；wrapper 依賴本機 registry、設定檔與 watcher，host 不得把 DSP 移到遠端節點；
- processor FUID 固定為 `(0x7C8A3D91, 0x0E6F4B27, 0xB1D8A45C, 0x92F36710)`；
- controller FUID 固定為 `(0x3F2B86E4, 0xC95047AD, 0xA67E19D2, 0x548CB301)`。

已發布後不得重用或變更這兩個 FUID。

MVP 只有一個公開參數 `Bypass`，穩定 ParamID 為 `kMonitorBypassParamId = 0x00010000u`，預設為 off，並以 `ParameterInfo::kIsBypass` 宣告。Host automation、component state 與 controller state 必須保存它。Bypass 時 float32／float64 都要輸出乾聲，不得讓舊的 FilterEngine buffer 留在輸出。

### Audio bus 與 sample size

MVP 接受一組 main input 與一組同 layout 的 main output，不建立 sidechain。Bus arrangement、channel pointer table、最大 block buffer 與 FilterEngine 初始化都在 `setupProcessing()`／`setBusArrangements()`／`setActive()` 等非 `process()` 階段完成。只接受 VST3 與 Windows 共同定義的 18 個 speaker bits，並把它們傳給 `FilterEngine`；VST3 mono 另映射到 front-center。Wide、Ambisonics 等 Windows mask 以外的配置會直接拒絕，不能默默改用預設聲道名而把校正套錯聲道。

`canProcessSampleSize()` 必須同時接受 `kSample32` 與 `kSample64`。`process()` 依 `symbolicSampleSize` 直接把 `channelBuffers32` 或 `channelBuffers64` 交給既有的 `FilterEngine::process(float**...)`／`process(double**...)`，不可只把 double 降成 float。Host 送出超過已宣告 `maxSamplesPerBlock` 的資料時安全輸出乾聲並回報可診斷狀態，但 callback 不得重新配置或重新初始化。

### 合成 device identity 與設定

VST3 host 沒有 Windows endpoint identity。Wrapper 在初始化前必須以以下固定值呼叫 `FilterEngine::setDeviceInfo(false, true, ...)`：

- `kMonitorDeviceName = L"Hibiki EQAPO Monitor VST3"`；
- `kMonitorConnectionName = L"DAW Monitor Insert"`；
- `kMonitorDeviceGuid = L""`；
- `kMonitorDeviceString = L"DAW Monitor Insert Hibiki EQAPO Monitor VST3"`。

空 endpoint GUID 是刻意的：不得把 DAW monitor 偽裝成 Windows render endpoint。`Device:` 可用固定 device string 對此 host 分流；響度校正則應使用明確的手動 `Volume`。`Binding Single` 不得暗中退回 Windows 預設端點，`Binding All` 也只能代表使用者有意選擇的 Windows 全域音量，不能宣稱它等於硬體 ASIO 或類比監聽音量。

例如，可只讓一段手動音量的公式響度校正套用到 monitor wrapper，然後恢復後續設定的 device scope：

```text
Device: DAW Monitor Insert Hibiki EQAPO Monitor VST3
LoudnessCorrection: Schema 1 Model FormulaLoudnessV1 Binding Single State 1 ReferenceLevel 80 ReferenceOffset 0 Attenuation 1.0 Volume -38.0
Device: all
```

`Device:` 採用上述穩定的合成 device string；結尾的 `Device: all` 很重要，否則同一檔案後續的命令也會留在這個 scope。`Volume -38.0` 只是範例，必須改成目前實際監聽音量；音訊介面的硬體旋鈕、介面內建 DSP/mixer 或類比衰減改變時，外掛無法自動得知，使用者也必須同步更新 `Volume`。

MVP 沿用安裝目錄登錄的主要 `config.txt` 與 FilterEngine 背景 reload，不在 plugin state 複製整份 EQ 設定。Plugin state 只保存版本與 bypass；設定解析、檔案監看及 engine 重建都不可在 audio callback 進行。

共用設定不得透過 `VSTPlugin:` 或 `OutProcVSTPlugin:` 再載入 Monitor module 本身。載入器以 Windows volume serial 與 128-bit file ID 比對目前 module，讓直接路徑、junction、symbolic link、hardlink、8.3 與 `\\?` 別名都在行程內載入或建立行程外 host 前失效安全地被拒絕；此 guard 是防止遞迴與無界子行程鏈的第二道防線，不取代正確的 `Device:` scope。

### 即時執行緒契約

Wrapper 自己擁有的 `process()` 程式碼只能做有界的 parameter-queue 掃描、乾聲複製、預配置的 channel dispatch、atomic 狀態讀取與 `FilterEngine::process()`。下列禁止事項是 wrapper callback 邊界的契約；使用者設定另外載入的第三方 VST、行程外處理或其他 filter 仍須各自驗證 deadline，不能據此宣稱整條任意設定鏈皆 lock-free：

- `new`／`delete`、heap allocation、容器擴張或字串建立；
- mutex、condition variable、sleep、wait 或任何可能阻塞的同步；
- 檔案、registry、COM、DLL 載入、網路、程序或執行緒 I/O；
- logging、UI、設定載入、FilterEngine 初始化或裝置查詢。

初始化失敗、未支援 layout、尚未取得有效 engine、offline mode 或 bypass 時皆輸出乾聲。錯誤只以預配置 atomic 狀態交給非即時執行緒顯示。

`FilterEngine` 只有在 `currentConfig` 已發布有效 configuration 時才算 ready；僅完成 buffer 配置不足以表示成功。讀不到已登錄的 `ConfigPath`、主 `config.txt` 或初始設定解析失敗時，wrapper 不保存該 engine，並維持乾聲與 `EngineFailure` 診斷。

### Build 與安裝產物

初版只交付目前產品支援的 x64 bundle。建置 staging 位置固定為：

`build\VST3\Release\HibikiEQAPO\HibikiEQAPOMonitor.vst3\Contents\x86_64-win\HibikiEQAPOMonitor.vst3`

正式安裝位置固定為：

`%CommonProgramFiles%\VST3\HibikiEQAPO\HibikiEQAPOMonitor.vst3\Contents\x86_64-win\HibikiEQAPOMonitor.vst3`

`MonitorVST3\MonitorVST3.vcxproj` 納入 `HibikiEQAPO.sln` 與 `build-local-x64.ps1`，並明確傳入受建置腳本驗證的 `VST3_SDK_ROOT`、`MONITOR_STATIC_LIB_DIR` 與 `MONITOR_VC_RUNTIME_DIR`。Monitor 專用的 vcpkg install root 使用 `x64-windows-static-md`：FFTW、libsndfile、FLAC、Ogg/Vorbis、Opus、mpg123 與 LAME 全部靜態連入外掛，不能在共享的 DAW process 暴露 `sndfile.dll`、`fftw3.dll` 或 codec 的通用 basename。這避免 delay-load 與轉移相依 DLL 綁到另一個插件先載入的 ABI-incompatible module。`scripts\test-monitor-vst3-binary.ps1` 同時檢查 PE normal imports 與 delay imports，Release staging 在複製前必須通過這個 gate。

靜態連結的代價是較大的 module、額外的相依建置時間與獨立 package cache，也把第三方授權審查帶到最終 binary 的發布流程。正式發布此外掛前，維護者必須依實際鎖定版本逐一確認並隨產物提供各授權所要求的 notices、對應原始碼或 relink 材料；本 ADR 不構成法律判斷。主 NSIS 目前不散布此外掛，不能把「本機可建置」當成已完成這個 release gate。

Release 建置 payload 只包含外掛主模組與四個 app-local VC runtime；manager 安裝後會再新增自己的 `Contents\Resources\hibiki-eqapo-monitor.json` ownership manifest。Debug 使用獨立輸出目錄，只供開發，不得交給安裝管理腳本。`scripts\stage-installer-x64.ps1` 會先對清理目標做 repo containment 與完整祖先 reparse 檢查，再把這五個 payload 檔案精確 staging 到：

`Setup\lib64\VST3\HibikiEQAPOMonitor.vst3\Contents\x86_64-win`

這個 staging 路徑只是建置與完整性預檢產物。主 NSIS 不會自動安裝、更新或移除外部 `%CommonProgramFiles%\VST3` tree，也不會把 Monitor VST3 納入 Windows「已安裝的應用程式」交易。這避免現有 `$INSTDIR` rollback/uninstall 在不知道 DAW 檔案佔用與外部 bundle ownership 時跨目錄破壞。

明確需要此功能時，使用 64-bit PowerShell 執行 `scripts\manage-monitor-vst3.ps1` 的 `Install` 或 `Uninstall`。腳本只接受 Release bundle，且只操作 `HibikiEQAPO\HibikiEQAPOMonitor.vst3` 這個固定目標；安裝時對每個檔案寫入長度與 SHA-256 ownership manifest，更新或移除前必須再度驗證目錄形狀、manifest、hash、reparse point 與檔案佔用狀態。舊 bundle 與新 bundle 透過同層固定的 `.new`、`.backup`、`.failed`、`.removed` transaction slots 交換，並以跨程序 mutex 序列化；下一次執行會依正式 path 與固定 slots 恢復中斷於 atomic rename 之間的交易。任何未被 manifest 擁有、已被竄改或含額外檔案的 bundle 都拒絕覆寫或刪除。DAW 佔用檔案時必須先關閉 DAW 再重試。

腳本預設安裝到 `%CommonProgramFiles%\VST3\HibikiEQAPO`；`-DestinationRoot` 只供測試、管理式部署與明確的自訂 VST3 root。目錄 rename 成功就是 commit：更新後驗證失敗會先把新 bundle 移到固定 `.failed` 再還原 `.backup`；移除一旦移到 `.removed` 就視為已卸載，若後續逐檔清理失敗則保留精確 tombstone 並明確報錯，不會假裝能還原已部分刪除的 bundle。這個獨立安裝路徑不代表 tagged binary release 已經通過真實 DAW matrix。

## 後果與限制

- DAW 可繼續直接使用低延遲 vendor ASIO；外掛不需要也不擁有 ASIO SDK。
- FilterEngine 執行在 DAW process 內；FFTW/libsndfile/codec 已用靜態連結排除通用 DLL basename 碰撞，但未捕捉例外、第三方 VST/IR 問題或其他 ABI 衝突仍可能使 DAW 不穩定，因此 production 前需要多 host 掃描、載入、卸載、session restore 與長時測試。
- 靜態依賴會增加 module 體積、build/cache 成本與發布授權義務；未完成逐版本的第三方 notices、source/relink 材料檢查前，不得把本機 bundle 當成可再散布成品。
- 插在真正的 monitor slot 時，DAW export graph 本身不包含此外掛；offline dry bypass 只是補強，不是 routing 的替代品。
- 插在 master bus 時，realtime export 仍可能把校正燒入。這是不可隱藏的產品限制，不得以行銷文字淡化。
- Bypass automation 採 process-block 邊界，不是 sample-accurate；正式 host matrix 必須涵蓋 automation 與不同 buffer size。
- 任意 `Delay:`、`Convolution:`、`VSTPlugin:` 或行程外設定的動態 latency／tail 不會回報給 host；此限制也是只能放在 monitor-only path 的理由之一。
- 硬體 direct monitoring、音訊介面 mixer 與類比音量都在此外掛之外。
- 主 NSIS 與 Windows「已安裝的應用程式」不管理此外掛；使用者必須明確執行 ownership-aware manager，更新或移除前關閉 DAW。
- 本地可重現的 wrapper 建置、PE/self-load CI gate、一次性人工 validator 結果與虛擬目錄安裝交易都只證明工程路徑；真實 DAW 的 Monitor FX、session restore、長時穩定性與 real-time export 仍是正式二進位發布門檻。
- 32-bit DAW 不在 MVP 範圍；若未來支援，必須增加獨立 `x86-win` bundle binary 並重跑相同 callback 與 host matrix，不能讓 32-bit host 載入 64-bit module。

## 正式發布驗證門檻

以下是發布前必須以當次 binary 證據完成的 gate，不是僅因 ADR 狀態為 Accepted 就視為已通過；最新一次本機執行結果與可重現命令記錄在 `HANDOFF.md`：

- VST3 validator 通過 factory、bus、state、bypass、float32 與 float64 測試。這是人工 release gate；repo 雖追蹤 SDK 與 validator 原始碼，目前尚無受專案維護、可重現的 validator bootstrap/build script，因此 CI 不自動建置或執行它。維護者以 `scripts\test-monitor-vst3-validator.ps1 -ValidatorPath <受信任的 validator.exe>` 執行一致的 `-e` 測試；省略路徑時只找 repo `_build\vst3-validator-x64\bin\validator.exe`，不從 `PATH` 猜測工具。
- PE normal/delay import audit 證明外掛不再匯入 `sndfile.dll`、`fftw3.dll`、FLAC/Ogg/Vorbis/Opus/mpg123/LAME 等通用 codec DLL。
- 合成 device identity 能以 `Device:` 只選中 monitor host，且自動端點音量不會誤綁。
- offline mode 與 bypass 對所有支援 layout 都逐樣本輸出乾聲。
- `process()` 的 allocation/I/O guard、最差 callback 時間及 host unload 測試通過。
- 至少在一個具獨立 Monitor FX 的 DAW驗證正常 playback 有校正、offline export 檔案無校正；另以 real-time export 證明警語不可移除。

本 ADR 接受的範圍是 x64 source build、預設 staging 與獨立 ownership-aware manager；build 與 release workflow 只自動執行可由現有建置來源重現的 PE import audit 與 direct/hardlink self-load guard。VST3 validator 目前維持人工 release gate：不得依賴未受 bootstrap 管理的本機 `validator.exe`，也不得把一次本機通過誤寫成 CI 自動通過。真實 DAW 驗證門檻未完成時，不得把此產物宣稱為已通過所有 host 或已可正式發布。
