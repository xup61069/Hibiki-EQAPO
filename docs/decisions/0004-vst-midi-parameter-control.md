# ADR-0004：VST MIDI 參數控制

## 狀態

Accepted

## 日期

2026-09-05

## 背景

使用者需要用硬體 MIDI 旋鈕、推桿與按鍵控制 VST2／VST3 效果參數，且 Configuration Editor 關閉後仍要生效。Windows WinMM 提供 MIDI input 裝置列舉、開啟、啟動與 callback；相關契約見 [`midiInGetDevCapsW`](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-midiingetdevcapsw)、[`midiInOpen`](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-midiinopen)、[`midiInStart`](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-midiinstart) 與 [`midiInClose`](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-midiinclose)。

VST3 的 [`IEditController`](https://steinbergmedia.github.io/vst3_doc/vstinterfaces/classSteinberg_1_1Vst_1_1IEditController) 提供 normalized parameter 存取；[`ParameterInfo`](https://steinbergmedia.github.io/vst3_doc/vstinterfaces/structSteinberg_1_1Vst_1_1ParameterInfo.html) 提供穩定 ParamID、step count 與唯讀／隱藏旗標。

## 決策

每列以有版本、長度受限的 `MidiConfig` 保存一個 MIDI 裝置識別與最多 256 個 mapping。支援 CC、Note 與 Pitch Bend；來源包含特定頻道或任意頻道。Automatic 對 Note 使用 toggle，對 CC／Pitch Bend 使用 absolute。離散參數依 step count 量化。

VST3 mapping 使用 ParamID，目標只接受可見、非唯讀且宣告 `kCanAutomate` 的參數；VST2 沒有同等穩定識別，因此使用 parameter index 加名稱 guard。更換外掛或 VST3 class 前必須確認並清除不再安全的 mapping。壞掉、未知版本、重複來源或超限資料只停用 MIDI，不得停用 VST 音訊。

同一行程使用固定容量的 WinMM broker，讓多列與 Learn 共用每個實體裝置的單一 input。WinMM callback 只把短訊息以 atomic fan-out 寫入各訂閱者的固定容量 SPSC queue。裝置列舉、開關、重新連線、訂閱管理與字串處理都在 worker／UI 執行緒；audio callback 每個 block 只取有限數量的更新，不配置、鎖定、等待、記錄或存檔。VST3 的 input parameter changes 必須在進入即時處理前預配置／預熱。

同一列中的相同實體來源只能綁定一個目標；重新學習會取代舊 mapping，codec 也拒絕 any-channel 與特定頻道重疊造成的歧義。裝置識別使用 manufacturer、product、driver version、同名 ordinal 與名稱；driver version 改變時只能在 manufacturer/product/name/ordinal 仍一致時 fallback。

行程外 host 的 sidecar 格式攜帶有界的 parameter descriptor metadata，並保持舊版 sidecar 可讀。MIDI mapping 的唯一擁有者是 Editor row；行程外 GUI 回傳狀態不得用過期 mapping 覆寫新值。

Editor 終止行程外 GUI 時，必須先送出 `GuiExit` 並給 host 有限寬限時間，讓 host 在正常退出前擷取並寫回最後狀態；只有超時才可強制終止。強制終止前要同時核對 PID、process creation time 與 `EqApoOutProcHost.exe` 的 canonical 路徑，不能只信任可能殘留且遭 PID 重用的暫存 PID 檔。終止期間讀回的狀態若有變更，必須先更新 Editor model，才可開始 MIDI learn 的 temporary-audio transaction。

目前列已有 mapping 時，Editor 必須先確認設定檔已儲存、磁碟內容逐位元相符且沒有其他暫時音訊狀態，再使用與 A/B／校準相同的 durable recovery journal，暫時把該列序列化成沒有 `MidiConfig` 的版本。學習對話框必須先解構並關閉 WinMM handle，才可還原原始檔案；只有還原成功且使用者沒有選擇保留外部修改時，才能把新 mapping 寫回 Editor model。寫入中斷或 Editor 當機時，既有暫時音訊復原流程負責還原或提示衝突。

## 後果與限制

- 這是參數控制，不把 MIDI event 傳給樂器型外掛。
- MIDI driver 可能只支援單一行程。Broker 會消除同一行程內的重複開啟，Editor 也會自動暫時釋放目前列的 runtime mapping；其他 APO 列或其他程式仍可能占用裝置，UI 必須顯示 Busy 並持續重試。
- 外掛本身仍在即時音訊路徑執行；MIDI 有界化不能保證第三方 parameter setter 一定即時安全。
- 真實硬體、拔插、休眠與多列 fan-out 需要另做裝置測試；無硬體環境只能驗證 codec、queue、生命週期與合成訊息。
