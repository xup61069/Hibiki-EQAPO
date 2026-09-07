# AI 交接快照：Hibiki EQAPO／透明 ASIO proxy

最後更新：2026-09-07（Asia/Taipei）

## 當前結論

- PR #7 已以 merge commit `9aab9b16c98e1efc529b50c97ec3a29bef33e323` 合併至 `main`（不是 squash merge）。`v3.1.0` 標籤已推送，但 release run `34064319013` 失敗，未產生 GitHub Release。修復位於 `codex/release-ci-repair`；目前工作是修復 Windows checkout 換行與測試 ASIO SDK 目錄缺漏。
- x64 **Hibiki EQAPO** ASIO proxy 已接入 source、local build、installer staging／registration／rollback／uninstall 與 binary gates。使用者在安裝時選底層原廠 driver，再於每套 DAW 的全域 Audio Device 選一次 Hibiki EQAPO；不需要在每個專案掛 VST。
- [ADR-0007](docs/decisions/0007-transparent-asio-proxy.md) 仍為 **Proposed**。Fake-vendor 與自動化驗證已完成，但真實 vendor ASIO driver＋DAW matrix 尚未完成；因此 proxy 是 experimental，不得宣稱普遍相容或正式 release-ready。
- `HibikiEQAPOMonitor.vst3` 只保留為特殊 monitor-only 工作流的進階 fallback；主 NSIS 不部署、更新或移除它。
- 2026-09-06 已在使用者明確授權下，將 unsigned experimental 3.0.7 x64 installer 實機安裝至既有相容路徑，底層 driver 為 `Universal Audio Volt`、TargetCLSID `{7FA0A3EC-EBB7-4249-9CDC-F5474EE19B74}`。當時 ASIO／COM registration、driver 檔案、音訊服務與 recovery journal 均驗證通過；尚未進行 DAW playback。沒有安裝 fallback VST3。這是歷史部署證據，不能視為 3.1.0 發布驗證。

接手時先讀 `AGENTS.md`，再執行：

```powershell
.\scripts\agent-status.ps1 -Fetch
```

若 Git、build、artifact 或測試與本檔不同，以即時結果為準。ASIO 完整工程報告見 [docs/2026-09-06_hibiki-eqapo-asio-proxy-report.md](docs/2026-09-06_hibiki-eqapo-asio-proxy-report.md)。

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
