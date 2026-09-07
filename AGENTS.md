# AGENTS.md — AI 協作者操作規範

本檔適用於整個 repository。AI 開始工作時先讀本檔，再依任務路由讀必要文件；`HANDOFF.md` 只是有日期的暫時快照，必須以即時 Git 狀態複核。對使用者以台灣繁體中文溝通，程式碼、註解與 commit message 則沿用專案既有的英文風格。

## 先確認你在正確的 repository

正式來源是 <https://github.com/xup61069/loudness-correction-apo>。本機上層目錄可能同時包含數個舊 clone、worktree、安裝檔、dump 與另一個完全無關的 Git repository；不可從上層目錄直接執行提交。

開始前在 repository 根目錄執行：

```powershell
.\scripts\agent-status.ps1 -Fetch
```

此命令會驗證 repository 根目錄、solution 與 `origin`，顯示即時版本、branch、HEAD、相對 `origin/main` 的 ahead/behind、dirty files 及所有 worktree。網路失敗時要揭露 fetch 未完成，不可把本機快取的 `origin/main` 當成最新遠端。

`origin` 必須指向 `xup61069/loudness-correction-apo`。若 `git rev-parse` 回到 `G:\AICODE`、remote 是 `ae-effects-db`，或 repo 根目錄不含本專案的 `HibikiEQAPO.sln`，立即停止；那是錯誤的上層 repository。上層針對 After Effects 資料庫的 `AGENTS.md` 不適用於本專案。

工作樹若已有修改，全部先視為使用者或前一位 AI 的工作。先讀 diff、保留它們，只明確 stage 本次檔案；禁止用 `git add .`、`git add -A`、破壞性 reset 或 checkout 清理工作樹。也禁止 `git clean -fdx`／`git clean -xdf`：ignored 路徑可能含使用者自行提供的 IR、耳機校正資料與外掛，不是可任意刪除的 build cache。

## 專案定位

Hibiki EQAPO 是 Mixomo `EqAPO64_with_VST3_support` 的非官方 Windows x64 相容 fork，產品功能包括系統層 Equalizer APO、透明 DAW 監聽用 x64 ASIO proxy、x64 VST2/VST3 效果器支援、兩個完全獨立的響度校正元件、Configuration Editor、Device Selector 與更新程式。`LoudnessCorrection:` 是公式版；`LoudnessCorrectionOriginal:` 是原始 Mixomo 雙棚架版，不得實作成同一元件的模式。

不可把本專案描述成官方 Equalizer APO 發行版，也不可宣稱響度功能符合、通過、獲認證或獲背書於任何標準。對外措辭與資料授權以繁中主文件 `README.md`、英文翻譯 `README.en.md` 及 `NOTICE.md` 為準；`README_zh-TW.md` 只保留舊連結導向，不是另一份內容來源。

需要即時版本時執行：

```powershell
.\scripts\get-project-version.ps1
```

`version.h` 是產品版本來源。`vcpkg.json`、release workflow 的手動預設值、`CHANGELOG.md` release heading 與版本契約測試是發布時必須同步的明確副本；README、AGENTS 與一般腳本不得硬編「目前版本」。

README 只描述目前功能、操作與限制，不放 `What's new`／「更新重點」等 release log。版本歷史唯一放在 `CHANGELOG.md`；尚未發布的已合併變更放 `Unreleased`。

## 文件來源與任務路由

真實性優先順序為：即時 Git／測試結果 → `AGENTS.md` 長期契約與 `docs/decisions/` 架構決策 → README 現行使用者行為 → `CHANGELOG.md` 版本歷史 → `HANDOFF.md` 暫時快照。下層文件不得覆蓋上層事實。

| 任務 | 額外必讀 |
|---|---|
| 接續未完成工作 | `HANDOFF.md`，並逐項以 Git 與測試複核 |
| 一般程式修改／PR | `CONTRIBUTING.md`、相關程式與測試、相關 ADR |
| 產品行為、設定、UI 或使用者文件 | `README.md`；需要同步英文時再讀 `README.en.md` |
| 響度資料、授權或對外聲明 | `NOTICE.md` |
| 原版響度校正或舊設定遷移 | `docs/decisions/0003-separate-original-loudness-component.md` |
| VST MIDI parameter control | `docs/decisions/0004-vst-midi-parameter-control.md` |
| IR 卷積、動態 callback frame 或 FIR 重建 | `docs/decisions/0005-ir-convolution-runtime.md` |
| ASIO proxy、driver 登錄、callback bridge 或 DAW 監聽 | `docs/decisions/0007-transparent-asio-proxy.md` |
| 安全問題或支援政策 | `SECURITY.md` |
| 第三方相依與建置來源 | `third_party/README.md` |
| 發布／升版 | `CHANGELOG.md`、`Release checklist.txt`、release workflow；不得略過任何一步 |

## 結構速查

| 路徑 | 職責 |
|---|---|
| `filters/loudnessCorrection/` | 響度輪廓、濾波擬合、音量追蹤、係數更新與即時 DSP |
| `Editor/guis/` | Configuration Editor 的 Qt GUI、設定遷移、校準與工作室面板 |
| `EqualizerAPO/` | 系統 APO runtime |
| `HibikiEQAPODriver/` | x64 ASIO proxy runtime、COM server、sample codec 與原生測試 |
| `EqApoOutProcHost/` | 實驗性行程外 VST host |
| `Benchmark/` | 原生 parser、DSP、交接與安全契約測試 |
| `Setup/` | NSIS installer；產生的 `.exe`／`.sha256` 不進 Git |
| `scripts/` | 相依套件、建置、staging、runtime、UI 與公開歷史檢查 |
| `tests/` | Python 靜態契約與回歸測試 |
| `docs/decisions/` | 不應隨短期分支失效的架構決策與取捨 |
| `.github/workflows/` | 正式 build 與 release 流程 |
| `third_party/` | bootstrap 管理的相依套件；不要手動提交其建置輸出 |

## 不可破壞的工程契約

### 即時音訊

- Audio callback 內不得配置或釋放記憶體、等待或取得可能阻塞的鎖、寫 log、呼叫系統 API，或執行不可預測的 I/O。
- ASIO proxy 必須把 DAW output 指向 proxy 自有雙 buffer，input 才可原樣指向 vendor buffer。每次序列化的 vendor callback 進入時，只把已完成的「上一個 host block」stage 複製到 vendor 目前 half（缺漏時清零）並在 commit 後呼叫 vendor `outputReady()`，再把目前 half 交給 host；host 的 `outputReady()` 只代表該 host block 已填完，絕不可 1:1 轉送給 vendor。`directProcess=true` 可在 host callback 返回後完成 DSP 與 private stage；`directProcess=false` 通常等 host worker 呼叫 `outputReady()` 才做相同工作。若 host 明確忽略建議、在 `ASIOFalse` callback 內同步完成並呼叫 `outputReady()`，可在同一 stack 執行已預備且 callback-safe 的 DSP 以保留相容性，但仍只能讀 host buffer、寫 private stage，絕不碰 vendor buffer。啟動前的 host half-1 pre-roll 也必須先 DSP/stage，vendor 兩個 half 先清零，並向 DAW 回報固定增加一個 buffer block 的 output latency。queue overrun、沒有 pending token 的 completion、host 例外、stage deadline miss 或 DSP claim 競爭等可觀測錯序一律進入 terminal fail-safe；只有仍能證明目前 vendor half 可寫時才清成 silence，ownership 不可證時必須 no-touch，不得猜測或碰錯 buffer。Host 必須對每個非同步 block 恰好一次、依 FIFO 呼叫 completion；因 `outputReady()` 沒有 index/generation，舊 duplicate 若落在下一個有效 token 視窗就無法辨識，明確不受支援。Callback dispatcher 只能有一個 active owner，並只支援 vendor 序列化 callback；登錄、COM 啟用、設定載入、sample-type 檢查、scratch 配置與 teardown 全部留在非即時路徑。DSD 與無明確 speaker mapping 的多聲道不得靜默當作 PCM stereo 處理。
- 第一版 ASIO proxy 對 host 只宣告 vendor 前一或兩個 output channels，且 `createBuffers` 必須用相同範圍驗證，不能宣告完整埠數後再拒絕。若失敗 start 或成功 stop 後仍有 `directProcess=false` producer，該 prepared allocation 永久不可重用；dispose 必須 pin 完整 host-visible arena、instance 與 module 到 process 結束，要求重開 DAW，不能為了回收而形成 use-after-free。Vendor `outputReady()` 只是一個 optional hint，失敗只能停用提示並正確回報音訊仍繼續。
- WinMM MIDI callback 也只可寫入預配置的固定容量 queue；同一行程內每個實體裝置只能由固定容量 broker 開啟一次，裝置列舉、開關、重連、字串、設定編解碼與 fan-out 訂閱管理都必須留在非即時執行緒。VST audio callback 每個 block 只可消費有界數量的 parameter change；VST3 `IParameterChanges` 的 queue 與 point storage 必須在開始處理前配置／預熱。
- IR／GraphicEQ 卷積 callback 遇到實際 frame count 與目前 bank 不符時，只能發布 lock-free 尺寸請求並複製乾聲；不得仿照舊上游修正在 callback 重新讀檔、配置、建 FFTW plan 或初始化 HybridConv。IR cache、固定尺寸 bank、每聲道最多 4,096 個 HybridConv partition、pending／retired 生命週期與 10 ms 乾濕淡入依 ADR-0005；超過 partition 上限只能 fail safe 保持乾聲。
- 昂貴的輪廓擬合、端點查詢、峰值搜尋與係數準備必須在非即時路徑完成；callback 只消費已發布且生命週期安全的狀態。
- 保留既有 raw → common-A 安全交接、預熱、淡入、雙 bank 係數交叉淡化及失敗時 bypass 的行為。
- Settled callback 會使用 `initialize()` 依 `maxFrameCount` 預先配置的 scratch buffer 走 section-major block 路徑；交接、預熱、crossfade 與 bypass fade 仍走 sample-major fallback。修改這兩條路徑時，必須保留原生 block/scalar、in-place/out-of-place 的逐樣本等價測試。
- 輸出餘裕掃描只降低校正分支，不得把共同的次聲頻路徑一起壓低。它不是 limiter；文件不可暗示能保證 sample peak 或 true peak。
- `VolumeFollow` 是在目前輸出（啟用校正時為 `L + headroom × correction(H)`；校正 bypass 時為 `L + H`）完整合成後才套用的獨立寬頻增益；不得併進 correction branch 的 headroom／`_outputGainLinear`，否則共同 A 域與 `Attenuation 0` 都不會正確跟隨音量。`State 0` 必須 bit-transparent 地旁路兩者，`State 1 Attenuation 0` 則仍要保留已啟用的跟隨。
- 跟隨增益變更使用預先配置、無配置的 10 ms amplitude ramp。所有聲道必須共享同一個 frame 位置；不得每處理一個聲道就把 ramp 多推進一次。Full／Fast、in-place／out-of-place、穩態／轉換與 ramp 中途換目標都要維持等價。
- 端點的 dB、0–1 scalar 與 mute 必須由非即時路徑取得成一致快照；曲線也在非即時路徑算成目標增益，只把該預先計算結果發布給 audio callback。callback 不得查詢 COM 或自行重算曲線。通知 callback 必須自行擁有生命週期安全的 change state，不可保存指向 `VolumeController` 成員的裸指標，因為 Windows 在 unregister 失敗後仍可能保留 callback。
- 自動 `VolumeFollow` 在冷啟動取得第一筆有效快照前必須靜音；執行期間讀取或重新綁定失敗時，校正分支依既有流程 bypass，但完整輸出的 follow gain 要保留最後一次成功衰減，不能跳回 unity。恢復後以 10 ms ramp 套用新值。

### 設定與相容性

- `LoudnessCorrection:`／`FormulaLoudnessV1` 與 `LoudnessCorrectionOriginal:`／`MixomoShelfV1` 的 command、parser、factory、runtime DSP、GUI、校準與狀態必須完全分離。兩者只可共用模型無關且已驗證安全的底層端點讀取；停用、重設或校準其中一個不得改寫另一個。
- 兩個校準對話框不論接受、取消或關閉都必須先停止循環測試音，再讓呼叫端復原 temporary-audio journal；復原時若使用者保留外部修改，必須丟棄本次量測，不得更新 model 或觸發即時儲存。
- 原版固定追蹤 Windows 預設 `eRender`／`eMultimedia`，不接受公式版的 `Binding`、`Engine`、`Volume` 或 `VolumeFollow`。新原版格式的 `Schema`、`Model`、`State`、`ReferenceLevel`、`ReferenceOffset`、`Attenuation` 都必填且唯一；錯誤時 fail closed。這不改變下方公式版舊設定對缺漏 `Attenuation` 的相容回退。
- 已標記的設定格式必須可 round-trip；必填欄位及 `Binding`／`Engine` enum 的無效值要 fail closed，重複的已知欄位也要 fail closed。`Attenuation` 與 `Volume` 為舊設定相容例外：缺漏或格式無效時分別回退 1.0 與自動音量。既有 parser 也會忽略不認識的額外 token 以保留 forward compatibility；不可順手改變這些語意。
- 未標記的舊響度設定不可依數值猜測模型；保留原文並 bypass，直到使用者在 Editor 明確選擇遷移方式。
- 新增欄位時必須定義缺省值、舊設定行為、序列化規則、未知值與重複值行為，並加入原生回歸測試。
- `VolumeFollow` 缺省與明確 `Off` 都表示 unity，序列化時省略 `Off`，以避免舊設定或正常端點重複承受 Windows 衰減。有效值只有 `Linear`、`Logarithmic`、`Windows`；無效或重複的已知欄位要 fail closed。Linear 使用 scalar `s`，Logarithmic 使用 `s²`，Windows 使用 `10^(d/20)`；自動模式的 `s`／`d` 來自端點 scalar／dB，手動模式的 `d` 是 `Volume`，`s` 則由 `clamp((Volume + 100) / 100, 0, 1)` 取得。自動端點 mute 永遠是零增益。
- 不得讓 UI 預覽、離線分析與實際 runtime 對同一份已儲存設定產生不同語意。
- 公式版啟用 `VolumeFollow` 時，輪廓聆聽衰減使用非靜音 `20 log10(g)`，與校準、工作台預覽一致；Off 才用端點／手動 dB。背景更新門檻依有效聆聽 dB 比較，scalar-only 可能需要重算線性／平方輪廓，mute-only 不需重算。UI 的跟隨目標是計算值而非 runtime 遙測，來源失聯不得假稱已知 APO 的最後增益。
- VST `MidiConfig` 壞掉、未知、超限或有重疊來源時只停用 MIDI，不得停用 VST 音訊。VST3 綁定以 ParamID 定位，且只可提供可見、非唯讀、具有 `kCanAutomate` 的參數；VST2 以 parameter index 加名稱 guard。行程外 sidecar 不得用舊 mapping 回寫 Editor row。更換外掛或 class 前必須確認並清除 parameter-specific state。
- 已有 `MidiConfig` 的目前列進入 MIDI learn 前，必須用 durable temporary-audio journal 暫時序列化成無 mapping 版本，讓 audio host 釋放只允許單一 client 的 WinMM 裝置。Learner 必須先關閉 handle 才還原原始檔；還原失敗或使用者保留外部修改時不得套用新 mapping。其他列／行程的占用仍只能顯示 Busy 並重試。
- `Convolution:` 是穩定設定指令，UI 顯示名稱是「IR 卷積」。相對路徑只以設定檔目錄解析；runtime 只接受與裝置相同取樣率及安全尺寸內、完整且有限值的 IR。Editor 的「重建相符 FIR」是使用幅度響應的手動 minimum-phase 重建，不是保留相位／延遲的 resampler，也不得在文字輸入或選檔時自動執行。
- 離線分析與自動前級的 freshness 判斷若涉及自動音量，必須連同端點 identity、dB、scalar、mute 及來源可用狀態一起比對；只比 dB 可能接受已靜音、曲線不同或失敗後恢復的過期結果。

### Installer、資料與供應鏈

- Installer 會使用既有 Equalizer APO 路徑與登錄位置；升級、rollback、uninstall 只在可拋棄的 Windows 測試環境驗證。
- ASIO proxy 只能登錄自己的固定 CLSID 與 `Software\ASIO\Hibiki EQAPO`，不得改寫原廠 driver 的 CLSID、`InprocServer32` 或檔案。Enumeration key 要在 target 與 COM server 都已成功後最後發佈；升級、rollback 與 uninstall 都必須驗證 ownership 並用耐中斷 journal。
- 不得刪除或覆寫使用者的設定、IR、耳機校正資料或其他非本產品擁有的檔案。
- 不得提交 credentials、installer、dump、proprietary plug-in、未確認可再散布的聲學資料或第三方資料集。
- `scripts/test-public-history.ps1` 會掃描整段可達 Git 歷史；禁入資料即使後來刪除，仍會污染公開歷史。
- GitHub Actions 必須以完整 commit SHA 固定；第三方建置 job 不得持有 repository write token。

### UI 與翻譯

- UI 變更至少檢查英文、`zh_CN`、`zh_TW`，並包含一般／深色／高對比與常用 DPI。
- `zh_TW` 使用台灣用語且不可留下 unfinished 字串。
- 追蹤中的 `.qm` 必須由相符的 `.ts` 重新產生；不可只提交其中一邊，也不可順手重排或正規化整批翻譯檔。

## 驗證階梯

所有變更至少執行：

```powershell
python -m unittest discover -s .\tests -p "test_*.py" -v
git diff --check
```

C++、DSP、project wiring 或 installer 變更再執行：

```powershell
.\scripts\build-installer-x64.ps1 -Configuration Release
.\scripts\test-runtime-loudness.ps1 -Configuration Release
```

響度效能變更另以 Release build 執行可重現的原生量測；數字必須連同取樣率、聲道數與 batch size 記錄，不能只寫「更快」：

```powershell
$previousPath = $env:Path
try {
    $env:Path = (Resolve-Path '.\Setup\lib64').Path + ';' + $previousPath
    .\Benchmark\x64\Release\Benchmark.exe --nopause --loudness-performance
} finally {
    $env:Path = $previousPath
}
```

行程外 VST host 變更再執行：

```powershell
python -m unittest discover -s .\tests -p "test_outproc_vst_lifecycle.py" -v
```

UI 變更再執行並人工檢查產物：

```powershell
.\scripts\capture-ui-regression.ps1 -Configuration Release
```

準備 PR 或 release 時再執行：

```powershell
.\scripts\test-public-history.ps1 -Revision HEAD
```

Runtime 測試依賴已建好的 `Benchmark\x64\Release\Benchmark.exe` 與 staged runtime，因此要在 Release build 後執行。任何因權限或環境而 skipped 的測試都要在交接中逐項列出，不能寫成「全部通過」。

修改響度音量追蹤時，原生矩陣至少要涵蓋缺省／明確 Off、三種曲線、automatic／manual、mute、冷啟動無快照、執行中失敗與恢復、`State 0`、`Attenuation 0`、單聲道／立體聲、in-place／out-of-place，以及 ramp 中途連續更新。另要保留接近中性輪廓不會固定降低 1 dB 的 Full／Fast 回歸。

## Git 與發布安全

- 先成功執行 `git fetch origin --prune`，再從最新 `origin/main` 建立短期 `codex/` 分支；不得直接把混雜工作樹推到 `main`。
- 只用 `git add -- <明確路徑>`，接著檢查 `git diff --cached --name-only` 與 `git diff --cached`。
- 每個 commit 聚焦單一主題；文件、功能、測試可依可審查性拆分，但不可遺漏功能所需的測試與使用者文件。
- 禁止 force push 到共享分支，禁止用 reset／checkout 或 `git clean -fdx` 清掉別人的改動或使用者資料。
- 未經使用者明確要求發布，不得建立或推送 `v*` tag。推送這類 tag 會觸發正式 GitHub Release。
- 版本更新必須同步 `version.h`、`vcpkg.json`、`CHANGELOG.md`、release workflow 的手動預設值，以及 `tests/test_loudness_safety_contract.py` 的版本契約；README 不寫目前版本，只在行為或遷移真的改變時更新。
- Build workflow 會在 `main` push、對 `main` 的 PR 或手動執行時觸發；普通功能分支 push 本身不代表 CI 已跑。

## 完工與交接

交付時必須清楚列出：

1. 修改了什麼，以及刻意沒碰哪些既有修改。
2. 實際執行的驗證、結果、skip 與未執行項目。
3. 仍存在的風險、限制與下一個可執行步驟。
4. branch、commit、push、PR、tag 與 release 的真實狀態。

若仍有未完成工作，更新根目錄 `HANDOFF.md` 的日期與快照；工作已完成時明確寫成「無 active WIP」。不要把易過期的 branch、dirty file 或一次性待辦塞回本檔的長期規範；可長期沿用的設計理由寫入 `docs/decisions/`。
