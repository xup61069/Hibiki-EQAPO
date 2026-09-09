# ADR-0007：以透明 ASIO proxy 提供 DAW 最終監聽校正

- 狀態：Proposed
- 日期：2026-09-06

## 背景

DAW 使用音訊介面的原廠 ASIO driver 時，Windows Audio Processing Object 路徑不在訊號鏈上，因此一般的系統 APO 校正不會作用於 DAW 監聽。把 VST3 插在每個專案的 master／monitor bus 需要使用者反覆掛載、檢查與移除，也有把監聽校正燒入 export 的風險，不符合本產品「安裝與一次設定後盡量無感」的目標。

ASIO 沒有系統預設 driver、標準 driver chaining 或可靠的 export/offline 狀態通知。任何通用方案都至少需要各 DAW 第一次選擇一次 driver；為覆寫原廠 driver 的 CLSID 或 `InprocServer32` 雖可偽裝成零設定，卻會形成 registry hijack，並在原廠更新、解除安裝與復原時變得不可預測，因此不採用。

## 決策

新增 x64、user-mode、in-process COM driver，對 DAW 顯示的產品名稱固定為 **Hibiki EQAPO**，CLSID 固定為 `{D47C55C9-3F7D-422F-86E9-32E170815D53}`。它使用自己的 ASIO 與 COM 登錄項目，從既有相容性登錄根 `Software\EqualizerAPO\ASIOProxy` 讀取使用者 override，否則讀取 installer 寫入的 machine default target CLSID，再以正常 COM 方式開啟該原廠 driver。

```text
DAW
  -> Hibiki EQAPO (own CLSID)
       -> selected vendor ASIO driver (vendor CLSID)
            -> audio interface
```

安裝程式只列舉 64-bit machine ASIO entries，排除自身。只有一個有效候選時可預選；有多個時由使用者明確選擇。無聲安裝可提供有效 `/ASIOCLSID={...}`、沿用既有有效 machine default、接受唯一候選，或以 `/NOASIOPROXY` 明確停用；若多個候選仍有歧義且未提供選擇，既有 silent install 仍可完成，但 proxy 保持未登錄，不能猜測或留下半設定的 driver。

Configuration Editor 的 x64 版本在頂端標頭提供 ASIO 原廠 driver 下拉選單。它與 proxy runtime 共用安全的 discovery／驗證路徑：只列出通過 x64 PE、有效 COM registration 與實體檔案一致性檢查的候選，且不載入或啟用原廠 DLL；自身 proxy 永遠排除。編輯器只會把使用者選定的 target CLSID 寫入目前使用者的 `HKCU\Software\EqualizerAPO\ASIOProxy\TargetCLSID`；選擇「Automatic／installer default」則只清除這個 user override，回到 installer 寫入的 machine default。這個控制面不改寫 vendor 的 `Software\ASIO`、CLSID、`InprocServer32` 或檔案。proxy 只在下一次 `IASIO::init` 讀取新選擇，因此下拉切換不是 hot switch；使用者必須讓 DAW 重新開啟音訊裝置，若舊的 host instance 仍持有 buffers 則完整重啟 host。非 x64 編輯器停用此控制。

每個 DAW 第一次將 audio driver 切換為 `Hibiki EQAPO` 後，DAW 自己保存的全域裝置設定會套用到後續專案。這不是每個專案都要插入的效果器。

## Callback 與 buffer 所有權

`createBuffers` 把 DAW descriptors 複製後交給原廠 driver，保存 DAW callbacks，並把 proxy callbacks 交給原廠 driver。成功後，input descriptors 才把 vendor 指標原樣交給 DAW；output descriptors 改指向 proxy 自有、整個 prepared lifetime 都固定不變的雙 buffers。所有 raw stage、planar decode／process scratch 與狀態槽都在這個非即時階段預先配置。原廠 output buffers 只允許序列化的 vendor callback 寫入，host callback 與 worker 永遠碰不到它們。

ASIO 慣例允許 host 在 `start` 前先填 output half 1。Proxy 在每次 start 前重建乾淨的 DSP 狀態、把該 pre-roll 資料處理到 private stage，並先清零兩個 vendor halves。第一次 vendor callback 進入時，把 pre-roll stage 提交到 vendor 目前可寫的 half；此時硬體正在取得的另一個 half 保持靜音。這使完整順序固定增加一個 buffer block，而不會把 pre-roll 錯放回原本 half 假裝零延遲。若 vendor 在 `start` 返回前同步 callback，stream 的 starting gate 也必須接受同一套序列；start 最後失敗時則先 close-and-drain，再清除 stage 並重建 DSP，讓 retry 不沿用已推進的濾鏡狀態。

每次 vendor buffer callback 必須先處理前一個 host sequence：stage 已完整發布時，將 raw bytes 複製到 vendor 本次可寫 half；stage 未完成、過期或 stream 已失同步時則清零該 vendor half。只有完成這次 commit 後，才可依快取能力呼叫 vendor `outputReady()`，接著才把目前 proxy half 交給 host。Host 的 `outputReady()` 只代表「本次 host buffer 已填完」，絕不可直接或延後 1:1 轉送給 vendor，否則通知會對錯 block，worker 較慢時也會落在 DMA deadline 之後。

`directProcess=true` 時，host callback 返回就以該 proxy buffer 完成 decode、`FilterEngine` 與 private raw stage。`directProcess=false` 時，callback 先把 half／generation／sequence token 放進固定容量 FIFO，外部 worker 的 `outputReady()` 取出對應 token 後完成相同工作。若 host 收到 `ASIOFalse` 卻明確選擇同步處理並在 callback 內呼叫 `outputReady()`，proxy 也在同一 stack 執行已預備、callback-safe 的 DSP；這是 Steinberg sample host／driver 會使用的合法組合。兩種路徑都只讀 host buffer、寫 private scratch/stage，不會寫 vendor buffer。

Proxy 必須向 host 回報支援 `outputReady()`，因為它同時是非同步 host 的完成訊號；在 initialized／prepared 且沒有 completion token 時，呼叫只用來 probe/cache vendor 能力，不能誤當 pre-roll 完成。底層 vendor 若回傳 `ASE_NotPresent`、其他非成功值或丟出例外，只停用其可選 ready 提示並留下正確診斷；音訊與 host completion 仍繼續，不能誤報為已靜音。

Host half 使用 Available → Announced → Processing 的 generation state；stage slot 使用含 sequence 的 reservation／writing／ready／reading／expired 狀態。Payload 必須先清除，最後才以 release 發布 Empty，避免舊 worker 與兩輪後的新 sequence 發生 ABA。前一 block 到下一次 callback 仍未 ready 時只送 silence；尚未 completion 的 host half 不能提早重用，遲到的 `outputReady()` 只能釋放自己的舊 generation，不能發布或清掉新資料。queue overrun、沒有 pending token 時收到 completion、過期 generation、host callback 例外、同時／重入 vendor callback、stage deadline miss 或 DSP claim 競爭等**可觀測**違規，都不得猜測另一個 half 或複製半成品，而要進入 terminal fail-safe。第一版因此只支援序列化、非重入的 vendor callbacks，以及每個非同步 block 恰好一次、依 FIFO 發出的 host completion。`outputReady()` 本身沒有 buffer index 或 generation；舊 duplicate 若刻意延遲到下一個有效 token 發佈之後，在 API 上與真正的新 completion 完全相同，proxy 無法辨識，這種 host 行為明確不在支援契約內。

Input-only stream 不建立 output staging、不增加 output latency，callbacks 與 input pointers 透明通過。`bufferSwitchTimeInfo` 的 host 回傳指標（包括 `nullptr`、原指標或替代指標）也必須在 callback lifetime 內原樣回給 vendor，不保存或替換。

Callback dispatcher 只允許一個 active buffer owner，使用原子操作發布／撤銷。每個 proxy instance 以同一個 64-bit atomic gate（最高位為 closed、其餘位元為 in-flight count）線性化 callback／worker entry 與生命週期 close-and-drain；static trampoline owner 亦使用可證明的 publish/drain 順序。`start` 失敗、`stop`、`disposeBuffers` 與 destructor 都必須先關閉並排空已進入的 callback 才能重設或釋放狀態。若 `directProcess=false` host producer 在失敗的 start 或成功的 stop 邊界後仍未結束，原 prepared allocation 永久禁止重用；dispose 必須把完整 host-visible arena、instance 與 module 固定到 process 結束，避免沒有 generation 參數的舊 producer 寫入 freed/reused memory，並要求重開 DAW。若 vendor `stop` 失敗，buffer ownership 已無法證明；proxy 必須永久停止碰觸 host/vendor buffers，也不得再送 `outputReady()`。此時硬體實際輸出無法由 proxy 保證，而由失敗後仍持有硬體的 vendor driver 決定；只有成功重試 `stop` 後再建立安全的 stream 邊界，或由 host 重開音訊裝置，才可恢復處理。

Callback 與 worker completion 都不得配置或釋放記憶體、取得鎖、等待、寫 log、讀 registry、做檔案 I/O 或呼叫控制面系統 API。registry、COM 啟用、設定載入、channel/sample-type 檢查、scratch 配置，以及需要等待 in-flight callback 的 teardown 全部留在非即時生命週期。同一個 host process 只允許一個已取得 static callback table ownership 的 prepared proxy instance；不同 DAW process 是否可同時使用仍由 vendor driver 的 multi-client 能力決定。

## 支援邊界

- 第一版只向 host 宣告並接受 vendor 的前一或兩個 output channels，作為 main-monitor mono/stereo pair；這讓會替所有 advertised channels 建立 buffers 的 DAW 不會先看到完整埠數再被拒絕。其他硬體 output pair 尚未提供選擇，多聲道在沒有明確 mapping 前必須拒絕，不能按 port number 猜測 Windows 5.1／7.1 順序。
- 支援 ASIO 定義的 PCM integer 與 IEEE float、little-endian 與 big-endian formats；integer/非原生格式使用 `createBuffers` 時預配置的 planar scratch 轉換，輸出必須飽和且處理 NaN/Infinity。
- DSD 不是可直接套用 PCM EQ 的資料，明確拒絕，不把位元資料誤認成 PCM。
- input buffers 原樣交給 DAW，不做校正；只處理實際建立的 output buffers。
- buffer size、聲道數、重複 channel descriptor、空 pointer、失敗的 channel info 或不支援格式都 fail closed；設定或 DSP 不可用但 host buffer ownership 明確時可提交原始乾聲。時序失效但仍能證明目前 vendor half 可寫時提交 silence；若 `stop` 失敗而 ownership 已無法證明，則完全停止碰觸 buffers 與 ready，不讓例外或未證明的寫入跨 ABI。

## 設定與匯出語意

Proxy 使用既有的 Hibiki EQAPO 設定引擎與 `config.txt`。一般沒有 `Device:` scope 的校正會自然生效；複雜的多裝置設定可用 proxy 的合成 device identity 分流。ASIO callback-safe policy 排除 active `VSTPlugin:`、`OutProcVSTPlugin:`、`OutProcGain:`、`OutProcBiquad:`、`VUMeter:` 與 original loudness correction；公式響度校正只有明確手動 `Volume` 時可啟用。inactive `Device:`／`If:` scope 與 `State 0` 可保留。初始不安全設定使 engine 維持乾聲；reload 不安全設定時保留上一份完整安全 configuration，不部分套用。

`getLatencies` 回報 vendor latency，並在有 output staging 時對 output 加上一個目前 ASIO buffer block；`future(kAsioGetInternalBufferSamples)` 成功時也對 `outputSamples` 做相同的飽和加法。`Delay:` 與 `Convolution:` 仍可執行，但 FilterEngine 本身再增加的 delay／tail 不另行回報 host。ASIO 沒有通用硬體音量 API，因此介面實體旋鈕與 direct monitoring 無法由此功能自動追蹤或處理。

一般 offline bounce 不會把音訊送到硬體 driver，因此天然不經 proxy，也不會燒入監聽校正。若 DAW 的 real-time export 實際經過 ASIO hardware，ASIO API 沒有通用 export flag，proxy 無法可靠自動辨認；文件必須保留此限制。

## 相容與品牌

核心檔名、既有 APO CLSID、`Software\EqualizerAPO`、預設安裝目錄與設定語法是升級相容識別，不因產品改名而任意更換。新 driver DLL 可使用 `HibikiEQAPODriver.dll`，但 ASIO driver list、registry `Description` 與產品名只顯示 `Hibiki EQAPO`。

Runtime 只接受 HKLM x64 ASIO enumeration 與 COM server，並在 COM activation 前要求 merged `HKEY_CLASSES_ROOT` 的 effective `InprocServer32` 仍與已驗證的 machine registration 指向同一實體檔案。候選 DLL 必須通過有界的 DOS／PE signature、AMD64、section count、PE32+ optional-header 與 DLL characteristic 檢查；不直接載入候選來做 architecture probe。

本專案透過固定版次 vcpkg port 使用 Steinberg ASIO SDK 2.3.4 的 GPLv3 路線，發行時必須提供完整 corresponding source、實際使用的 SDK headers、建置與安裝腳本並保留 notices。ASIO 不放進產品名；文件第一次使用寫成「Hibiki EQAPO is compatible with ASIO® technology.」，並附上：

> ASIO is a registered trademark of Steinberg Media Technologies GmbH.

## 未採用方案

- 每個 DAW 專案插入 monitor VST3：操作負擔與 export 誤用風險過高，只能保留為進階 fallback，不能是預設工作流。
- 覆寫 vendor ASIO registry／COM class：破壞原廠更新與解除安裝，且無法安全證明 ownership。
- 經 Windows shared-mode 或虛擬纜線再回硬體：改變使用者既有低延遲 ASIO 路徑並增加 routing、clock 與 latency 複雜度。
- 直接修改特定廠牌 driver：不通用，也不在授權範圍內。

## 驗證門檻

目前 fake vendor 與自動化驗證已完成；真實原廠 driver＋DAW gate 尚未完成。在此之前，ADR 維持 Proposed、功能維持 experimental，且前者不得取代後者。

在改為 Accepted 前至少必須完成：

- fake vendor driver 的 COM／IASIO forwarding、`directProcess=true/false` callback ordering、跨執行緒 completion、time-info、reset 與 output-ready 測試；
- 所有宣稱 PCM formats 的 round-trip、極值、clip、NaN/Infinity 與未對齊 buffer 測試；
- callback allocation/lock/I/O 靜態契約與 single-owner／teardown race 測試；
- x64 Release DLL 的 PE architecture、exports、normal/delay imports 與 self-load/recursion guard；
- installer 的唯一／多候選、silent options、upgrade rollback、uninstall ownership 與不碰 vendor key 測試；
- 至少一套真實 vendor ASIO driver 與一個 DAW 的播放、sample-rate/reset、buffer size、offline bounce、real-time export 與裝置拔插測試。
