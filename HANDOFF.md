# AI 交接快照：Hibiki EQAPO 雙精度開關修復與推播

最後更新：2026-09-11（Asia/Taipei）

## 本輪狀態：無 active WIP

- 本輪已徹底修復 Configuration Editor 頂部工作室標頭的「雙精度」開關：
  - 移去 `FilterTable::processingPrecisionEditable()` 中對 `Device` 與 `Stage` 的限制，使一般 Equalizer APO 設定檔（包含使用者的 `none.txt`）可直接由 GUI 開關修改精度。
  - 修正 `MainWindow::refreshWorkspaceActionState()`，開關勾選狀態真實反映 `doublePrecision`（64 位元勾選、32 位元未勾選），不再被 `!editable` 強制取消勾選。
  - 標頭開關標籤固定為「雙精度」（`Double precision`），不再被長字串「處理精度（請查看設定文字）」取代。
- 完成分支合併至 `main` 並已成功推播至遠端 `origin/main`（commit `0d713b9`）。
- 全套驗證通過：Python 399 項測試全數通過（398 passed, 1 expected skip）、90/90 UI 快照幾何與原廠約束零違規、DSP 即時契約測試通過、公開歷史檢查通過。

---

以下為歷史快照，不能代表目前工作樹或本輪驗證結果。

# 2026-09-08 歷史快照：Hibiki EQAPO／Studio UI

最後更新：2026-09-08（Asia/Taipei）

## 本輪交付

- UI 改版已完成，無 active WIP。使用原生 Qt：Hibiki 品牌標頭、訊號鏈空白引導、圓角模組、刻度旋鈕、橫向分析設定列與曲線光暈。一般新視窗為 1280×900；1024×768 仍有回歸覆蓋，已保存的視窗配置繼續沿用。
- 介面包含標頭、模組、選單、設定檔切換、按鈕、焦點、狀態通知、旋鈕及響應曲線等短動畫。動畫不延後控制項的實際數值或訊號，不使用永久更新計時器；高對比、Windows 減少動畫、快照模式或 `EQAPO_DISABLE_ANIMATIONS` 會停用。
- 正式審閱工作樹為 `G:\AICODE\iso226_2023\Hibiki-EQAPO-studio-ui`，分支 `codex/studio-ui-review`，基底為 `8a477f2666b3`。此分支沒有 DSP 變更。
- 原工作樹 `Hibiki-EQAPO` 的安裝測試修改與後續另一批 DSP 修改全部保留；其中亦保留本輪早期 UI 修改。請以本獨立分支審閱／整合 UI，避免把原工作樹混合提交。
- 沒有推送、建立 PR、tag 或 GitHub Release；沒有執行實機安裝／升級／卸載。產品版本未升版，產物是本機 UI 預覽建置。

## 驗證與產物

- 本獨立工作樹的 Python 全套：390 tests，0 failures，1 skipped。略過的是 `test_built_host_cold_starts_and_hands_off`，原因為建立 Windows global mapping 時得到 Win32 error 5；不代表該測試通過。
- `scripts/test-ui-motion.ps1`：10 passed，0 failed，0 skipped，驗證立即輸入、快速重入、隱藏／刪除與減少動畫。
- `scripts/build-installer-x64.ps1 -Configuration Release` 與 `scripts/test-runtime-loudness.ps1 -Configuration Release`：通過；`git diff --check`：通過。
- 相同 UI 原始碼完成 90 張 Windows 原生回歸圖（3 個程式、3 種色彩、100–200% DPI、150% 文字），並另檢查英文、zh_CN、zh_TW 的 9 張響度介面。圖與 manifest 保存在 `artifacts/studio-ui-regression/` 及 `artifacts/studio-languages/`。
- 專屬安裝檔：`artifacts/Hibiki-EQAPO-studio-ui-preview.exe`，16,118,988 bytes，SHA-256 `f7b68722a6c8376277d54501211be065eca3870ad1358442dcdb34a1c08e10eb`。此檔從獨立工作樹重新建置，沒有混入另一批 DSP 修改。
- 建置／測試紀錄在 `_build/studio-installer.log`、`_build/studio-runtime.log`、`_build/studio-tests.log`、`_build/studio-motion-tests.log`。第三方已安裝工具與套件使用本機共用 junction，專案的原始碼、中間產物、DSP libraries 與安裝檔則在本工作樹獨立建置。

---

以下是前輪歷史快照；本輪狀態以上文為準。

# 前輪紀錄：透明 ASIO proxy

最後更新：2026-09-07（Asia/Taipei）

## 當前結論

- **本輪音量修正與 3.1.1 發布已完成。** 包含公式版響度輪廓衰減與校準／工作台一致性修正、UI 目標 dB 數值與控制量顯示、清晰振幅曲線名稱、手動切回自動重新綁定保護；升級安裝檔已完成本機安裝測試，底層 ASIO 目標綁定至 MiniFuse。
- 前輪查核：PR #7 以 merge commit `9aab9b16c98e1efc529b50c97ec3a29bef33e323` 合併至 `main`；PR #8（commit `0184c5e`）修復 CI 換行與 ASIO SDK 目錄。本版以 3.1.1 釋出。
- x64 **Hibiki EQAPO** ASIO proxy 已接入 source、local build、installer staging／registration／rollback／uninstall 與 binary gates。使用者在安裝時選底層原廠 driver，再於每套 DAW 的全域 Audio Device 選一次 Hibiki EQAPO；不需要在每個專案掛 VST。
- [ADR-0007](docs/decisions/0007-transparent-asio-proxy.md) 仍為 **Proposed**。Fake-vendor 與自動化驗證已完成，但真實 vendor ASIO driver＋DAW matrix 尚未完成；因此 proxy 是 experimental，不得宣稱普遍相容或正式 release-ready。
- `HibikiEQAPOMonitor.vst3` 只保留為特殊 monitor-only 工作流的進階 fallback；主 NSIS 不部署、更新或移除它。
- 2026-09-06 已在使用者明確授權下，將 unsigned experimental 3.0.7 x64 installer 實機安裝至既有相容路徑，底層 driver 為 `Universal Audio Volt`、TargetCLSID `{7FA0A3EC-EBB7-4249-9CDC-F5474EE19B74}`。當時 ASIO／COM registration、driver 檔案、音訊服務與 recovery journal 均驗證通過；尚未進行 DAW playback。沒有安裝 fallback VST3。這是歷史部署證據，不能視為 3.1.0 發布驗證。

接手時先讀 `AGENTS.md`，再執行：

```powershell
.\scripts\agent-status.ps1 -Fetch
```

若 Git、build、artifact 或測試與本檔不同，以即時結果為準。ASIO 完整工程報告見 [docs/2026-09-06_hibiki-eqapo-asio-proxy-report.md](docs/2026-09-06_hibiki-eqapo-asio-proxy-report.md)。

## 2026-09-07 本輪音量跟隨驗證

- 修正：啟用跟隨後，初始化與背景輪廓改用非靜音 `20 log10(g)`，Linear／Logarithmic scalar-only 變更也會觸發必要更新；Off 與三條既有增益公式、設定 token、10 ms gain ramp、mute／失聯／State 0 行為不變。修正會改變先前算錯的線性／平方音色補償量。
- UI：明確顯示本列「APO 跟隨目標」及來源 dB／控制位置；不是量測或 APO 已載入的確認，不含 EQ／headroom／Windows／硬體衰減。曲線改顯示振幅線性、振幅平方、依 dB 衰減；手動 −50 dB 的 legacy scalar mapping 有明確說明。Single 從手動切回自動重新走 render／identity guard。Studio 使用同一公式，標明開窗快照，切回自動恢復快照 dB，未知／更換綁定時不繪製假定曲線。
- Windows 接管：維持 read-only，不強制 100%。現有 manual `Volume`＋`VolumeFollow Windows` 可做 APO dB 控制，但沒有可驗證的全路徑處理確認、獨立 fail-safe 增益與重開機復原；旁路 APO 會移除其衰減。設計理由與未來必要條件見 ADR-0002／README。未進行真實 DAW 或硬體聆聽驗證。

| 本輪 Gate | 結果／證據 |
| --- | --- |
| Native reproduction | 舊實作在 Full/Fast × Linear/Squared 四例失敗；修後三曲線 active-contour/post-gain 與 scalar-only synthetic publisher 對照通過（48 kHz、stereo、256-frame blocks；float 參數對照誤差門檻 `1e-7`）。`work/volume-baseline-runtime.log`、`work/volume-fixed-runtime.log` |
| Python | 390 run／389 pass／1 expected skip；`test_built_host_cold_starts_and_hands_off` 因 `Global\` mapping Win32 error 5 跳過。`work/volume-python-final.log` |
| Release installer | 完整 `scripts/build-installer-x64.ps1 -Configuration Release` 通過；`work/volume-installer-final.log`。初次增量建置 LNK1103 由完整 rebuild 排除 |
| Runtime | `scripts/test-runtime-loudness.ps1 -Configuration Release` 通過，包含既有 parser、失效安全、handoff、Full/Fast、near-neutral、ramp、block/scalar 等；`work/volume-runtime-final.log` |
| UI | 正式回歸矩陣 90/90 通過；額外 focused 21/21（EN／zh_CN／zh_TW × 三主題 × 100%／200%，另繁中三主題 150% text）。EN light 100%、簡中 dark 200%、繁中 light 100%／high-contrast text 150% 人工檢視可見完整新列、無字串重疊；`artifacts/ui-regression`、`artifacts/volume-follow-ui/focused-*` |
| UI test entry | `volume-follow` 是 test-only snapshot 情境；第一輪被白名單忽略的空白截圖作廢，已修正 whitelist、加契約，增量重建 snapshot Editor 後重拍 21 張。該修正不編入 production 分支；`work/volume-snapshot-entry-build.log`、`work/volume-focused-ui.log` |
| Translations | 四份 Editor `.ts`／`.qm` 同步重新產生；zh_TW 無 unfinished，新字串與 placeholders 契約通過 |
| Whitespace | `git diff --check` 通過 |

本輪釋出安裝檔版本為 3.1.1：`Setup/Hibiki-EQAPO-x64-3.1.1.exe`，16,111,899 bytes，SHA-256 `A209A447AE99E35FBBFCB45BED6D8D4981725E58F753ED03821DB75F18569907`。實機升級安裝與 Device Selector 重新綁定已測試通過。

## 不可破壞的 ASIO 契約

- DAW output descriptors 固定指向 proxy-owned double buffers；vendor buffers 只由序列化 vendor callback 寫入，host callback／worker 不碰 DMA buffer。
- 每次 vendor callback 先提交上一個完成 stage（或在 ownership 可證時送 silence），再送可選 vendor `outputReady()` hint，最後通知 host。Host `outputReady()` 是 completion，絕不可 1:1 轉送給 vendor。
- `directProcess=true` 在 callback return 後 stage；`directProcess=false` 使用預配置 FIFO token。Async completion 必須 exactly-once、FIFO；callback／worker 不配置、不等待、不取得鎖、不寫 log／registry／檔案。
- Output 固定增加一個 DAW-selected ASIO block；input-only 不增加。`Delay:`、`Convolution:` 等 filter 本身 latency／tail 仍未加入 host compensation。
- 只宣告並接受原廠前 1–2 個 outputs，作為 main-monitor mono／stereo pair。支援 18 種 little／big-endian PCM formats；DSD、三聲道以上與模糊 mapping 拒絕。
- 同一 process 只允許一個 prepared owner；第一版只支援 serialized、non-reentrant vendor callbacks。Deadline miss、queue overflow、無 token completion、generation 過期或可觀測重入會進入 terminal fail-safe。
- Ownership 仍可證時才清除 vendor half；真正同時重入或 failed stop 後 ownership 不可證時完全不碰 buffer／ready，硬體輸出由原廠 driver 決定。
- Stop／dispose／destructor 先 close-and-drain。Stop 成功但舊 async producer 未退出時，完整 arena／instance／module quarantine 到 process exit，必須重開 DAW；failed stop 只有成功 retry 建立新邊界後才可恢復。
- ASIO-safe config 拒絕 active VST、outproc、VU meter 與原版響度；公式響度僅允許固定手動 `Volume`。初次 unsafe config 保持 dry，unsafe reload 保留上一份完整安全 config。

## 品牌與相容識別

- 現行產品／UI／HTTP／installer／ASIO list 名稱使用 **Hibiki EQAPO**。COM friendly names 與 Update Checker User-Agent 也已更新；branding contracts 覆蓋這些表面。
- `EqualizerAPO.dll`、既有 APO CLSID、`Software\EqualizerAPO`、預設安裝路徑、設定語法、scheduled-task／named-object 技術 ID 與舊捷徑遷移字串是相容識別，不能為了改名任意更換。
- `Wiki/` 是 archived upstream Equalizer APO 文件快照，已用 `Wiki/README.md` 標示；目前使用方式以根目錄 README 為準。

## 本輪實機 installer 修正

- NSIS `EnumRegKey` 在 registry 列舉 EOF 可能回空字串卻不保留 `${Errors}`，造成只檢查 error flag 的 discovery 迴圈無限執行。五個 ASIO 列舉點均已加入完整 empty-name guard；deterministic 兩-driver、process-local HKLM discovery runtime 2／2 與 16 項 ASIO installer contract 通過。
- `RunEmbeddedProcessStopper` 的 PowerShell command 位於 NSIS 單引號參數內，原本內嵌的 `-eq '1'` 會破壞已有安裝版本的升級解析。現改為 `if([int]$env:EQAPO_PROCESS_PROTECT_INTERACTIVE -eq 1)`；production command 的 NSIS → nsExec → PowerShell runtime gate 2／2，以及其餘 40 項 installer contract 通過。
- 修正後 installer 已成功在實機完成升級式安裝。`Hibiki EQAPO` ASIO key、proxy CLSID／`InprocServer32`、Volt target、Windows Audio／Audio Endpoint Builder running 狀態與無殘留 installer recovery journal 均已確認；這只證明部署與註冊，不證明 DAW 音訊相容性。

## 2026-09-06 本機歷史驗證證據

| Gate | 結果 |
| --- | --- |
| Full Python suite | 387 run；386 pass；0 fail／error；1 expected skip（`Global\` mapping，Win32 error 5） |
| Final installer rebuild | Full native／Qt／staging gates 通過；最後一次 orchestration 因舊 installer process 鎖住輸出而停於 NSIS，清除那些精確 PID 後 production `makensis` rerun 成功。新增 test-only gates 後未覆寫 final installer |
| ASIO native tests | Core PASS；fake-vendor 41 named main tests＋isolated modes PASS |
| ASIO installer discovery | Deterministic two-driver runtime 2／2 PASS；16／16 ASIO installer contracts PASS |
| Process-stopper／general installer | Production NSIS quote-boundary runtime 2／2 PASS；general installer contracts 40／40 PASS |
| 實機 installer smoke test | PASS；Volt target、installed driver hash、ASIO／COM、音訊服務與 recovery journal 均驗證通過；未做 DAW playback |
| Runtime DSP | HybridConv、數值安全、FilterEngine reload／handoff、響度 runtime PASS |
| ASIO binary | AMD64 PE；4 exports；22 normal imports；0 delay imports |
| Monitor binary | AMD64 PE；3 exports；22 normal imports；0 delay imports |
| VST self-load guard | direct path PASS；hardlink PASS |
| Official VST3 validator | 537 passed；0 failed |
| UI regression | 90/90 snapshots、0 missing；繁中 light 100%、dark 200%、high-contrast large-text 抽查無亂碼／缺字／互疊。1024×768＋150% text 的 dense 編輯區需垂直捲動，截圖無法證明鍵盤／捲動可達性 |
| Whitespace | `git diff --check` exit 0；只有 Windows LF→CRLF notices |

唯一 Python skip 是 `test_outproc_vst_lifecycle.OutProcVSTLifecycleTests.test_built_host_cold_starts_and_hands_off`；目前執行身分不能建立 `Global\` named mapping，Win32 error 5。其餘測試不能取代該權限情境的實跑。

## 2026-09-06 本機歷史產物（3.0.7；非 3.1.0 發布產物）

| 產物 | Bytes | SHA-256 | 簽章 |
| --- | ---: | --- | --- |
| `Setup\Hibiki-EQAPO-x64-3.0.7.exe` | 16,105,896 | `3AE378D8995C8533A10FF07AFE6C756361C5013D39FE52707B993693416BB594` | NotSigned |
| `x64\Release\HibikiEQAPODriver.dll` | 5,179,392 | `6DD6672B390966728A547F0F352F5F116912F45B1049DEB10F739881A9E4C79F` | NotSigned |
| `build\VST3\Release\HibikiEQAPO\HibikiEQAPOMonitor.vst3\Contents\x86_64-win\HibikiEQAPOMonitor.vst3` | 5,146,112 | `492676b1fc914f69ea73c660566942b6880b71085ede4ad73598d5675f1a4d95` | NotSigned |

Driver source／stage 的 length 與 SHA-256 完全一致；Monitor source／stage 的五檔 payload 亦由 staging gate 驗證一致。Ignored tree 仍可能保留早期舊名 build artifacts；release workflow 只取 `Hibiki-EQAPO-*` 與目前 Hibiki staging，不得把舊快照當成 final artifact。

## Release blockers 與限制

- 至少一組真實 x64 vendor ASIO driver＋DAW 尚未完成播放、start／stop／failure、sample-rate／reset、buffer sizes、offline bounce、real-time export、record／overdub alignment、裝置拔插／restart 與 multi-client matrix。
- 固定增加一個 ASIO block；FilterEngine 額外 delay／tail 未完整回報；只支援第一個 mono／stereo output pair；DSD／multichannel 不支援。
- ASIO `outputReady()` 沒有 buffer index／generation，無法識別精準延遲到下一 token 後的舊 duplicate；此 host 行為不在支援契約。
- Hardware direct monitor、介面 mixer／DSP、實體旋鈕與類比輸出不經 proxy；手動 `Volume` 必須對應實際監聽 SPL。
- Offline bounce 是否繞過 proxy、real-time export 是否送到 hardware 仍由各 DAW routing 決定，必須真機驗證。
- Installer、driver 與 fallback VST3 都未簽章；正式散布前仍需完成 ASIO SDK／靜態連結第三方元件的 notices、corresponding source／relink materials 審核。
- 核心修改已合併至 main；ignored build、installer、回復安裝包與 `work/` evidence 保留於本機，不納入 Git。

## 接手順序

1. 重新確認 branch、HEAD、dirty files、實機安裝狀態與 final hashes；不要引用 ignored 的舊名 binary。
2. 若 source／project／Setup 有變動，重跑完整 Release installer、runtime、ASIO／Monitor PE、自載、官方 VST validator 與當下完整 Python suite，並保留 live ASIO discovery harness 與 installer contracts。
3. 在低音量、可復原測試環境完成真實 vendor＋DAW matrix，將具體 driver／DAW version、buffer／rate 與結果寫入新 evidence。
4. 只有真機 gate 全過後才能把 ADR-0007 改成 Accepted；否則正式 installer 必須停用／排除 proxy。
5. 使用者已授權推送與發布。既有 `v3.1.0` 標籤維持原 commit；任何包含修復的新發布須使用新版本，不能移動標籤或把不同 commit 的安裝包附到舊標籤。
