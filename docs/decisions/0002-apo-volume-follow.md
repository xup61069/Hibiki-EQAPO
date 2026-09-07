# ADR-0002：APO 音量跟隨必須和響度校正分支分離

## 狀態

Accepted

## 日期

2026-09-05

## 背景

響度校正原本只讀取 Windows 端點音量，用它決定感知音色補償量；實際寬頻音量仍由 Windows、硬體或後續路由負責。VB-Audio Matrix 等路由有時會讓端點 API 正常回報音量變更，音訊路徑卻沒有真的套用該衰減。此時低音量下的 EQ 曲線會改變，但總音量不會下降。

不能直接假設所有端點都有這個問題。一般 Windows 音訊路徑已經套用主音量，若 Equalizer APO 再做一次就會形成雙重衰減；外接擴大機與喇叭旋鈕也可能另有不可觀測的衰減。因此新功能必須明確選用、只讀取狀態，而且不能破壞舊設定。

這項增益會在系統 audio callback 執行。它必須延續 [ADR-0001](0001-loudness-engine-modes.md) 的即時限制，並在端點消失、COM 讀取失敗、靜音、校準及冷啟動都維持可預期的安全行為。

## 決策

1. **新增選用的 `VolumeFollow` 欄位。** 省略或 `Off` 都是 unity，也是序列化缺省值；舊設定因此不會改變音量。有效的明確值為 `Linear`、`Logarithmic` 與 `Windows`。未知值或重複的 `VolumeFollow` 會讓該響度項目 fail closed。
2. **三種曲線使用同一份已驗證快照，但不假裝重製 Windows 滑桿。** 對有限值先定義：

   ```text
   automatic d = clamp(reported endpoint dB, -100, 0)
   manual d = clamp(Volume, -100, 0)
   automatic s = clamp(reported endpoint scalar, 0, 1)
   manual s = clamp((Volume + 100) / 100, 0, 1)

   Off:         g = 1
   Linear:      g = max(10^(-100/20), s)
   Logarithmic: g = max(10^(-100/20), s²)
   Windows:     g = 10^(d/20)
   automatic endpoint muted: g = 0  （覆蓋三種啟用曲線）
   ```

   自動模式同時讀取端點的 dB、0–1 scalar 與 mute；手動模式直接以 `Volume` 作為 `d`。`Windows` 指「依端點實際回報的 dB 換算振幅」，不是從 slider scalar 猜測一條 Windows 曲線。Microsoft 將 endpoint scalar 定義為 audio-tapered 的正規化值，且明示 taper 可能改變，因此程式不把它當成穩定、可複製的 Windows UI 規格。[GetMasterVolumeLevelScalar](https://learn.microsoft.com/en-us/windows/win32/api/endpointvolume/nf-endpointvolume-iaudioendpointvolume-getmastervolumelevelscalar)；[Audio-Tapered Volume Controls](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audio-tapered-volume-controls)
3. **跟隨增益作用於完整輸出。** 訊號順序是：

   ```text
   active correction: output = g × (L + headroom × correction(H))
   identity / correction bypass: output = g × (L + H)
   ```

   `g` 不得併入只屬於 correction branch 的 headroom 增益。這可確保共同 `A = L + H` 域也會降低，並讓 `State 1 Attenuation 0` 仍可只作寬頻音量跟隨；`State 0` 則維持完整 bit-transparent bypass。
4. **不在 callback 查詢或配置。** 端點查詢、曲線計算及狀態發布在非即時路徑完成；callback 只消費已發布的目標增益，並用預先配置狀態進行 10 ms amplitude ramp。所有聲道共用同一個 frame 位置，避免立體聲各自推進 ramp 而產生聲像偏移。
5. **冷啟動與執行失敗採不同安全策略。** 自動跟隨在初始化取得有效快照時直接從該增益開始，不先經過 unity。若冷啟動完全沒有有效快照，啟用跟隨時先保持靜音，避免 Matrix 路由突然全音量。至少成功一次後，暫時讀取或重新綁定失敗會保留最後成功的 `g`；音色校正另外淡到未校正 A 域。來源恢復後，新的跟隨值以 10 ms ramp 套用。
6. **保持 read-only。** 程式不呼叫 endpoint volume setter，也不移動 Windows 音量。若實際路徑已套用 Windows、擴大機或喇叭衰減，跟隨增益會再相乘；使用者必須維持 `Off`。
7. **校準不解除必要的寬頻衰減。** 校準時暫時把音色補償設為平直，但已啟用的 `VolumeFollow` 繼續作用，避免測試噪音在 Matrix 路由突然變成全音量。
8. **移除固定 1 dB correction margin。** correction branch 只依偵測到的響應峰值衰減，再以完整傳遞掃描作必要的額外降低。跟隨關閉且輪廓為中性或近中性時，不應只因啟用功能就固定掉約 1 dB。這仍不是 limiter，使用者需自行保留播放餘裕。
9. **分析快照必須完整。** 自動音量的離線分析與自動前級 freshness 判斷要同時綁定端點 identity、dB、scalar、mute 與來源可用狀態，不能只比對 dB 後接受過期結果。
10. **輪廓與跟隨衰減共用聆聽級別。** 跟隨啟用時，初始化、背景更新、校準與 UI 預覽皆使用非靜音 `20 log10(g)`；Off 維持端點／手動 dB。Linear／Logarithmic 若只改 scalar，也必須以此有效 dB 判斷是否超過 0.05 dB 輪廓更新門檻；mute 單獨變化不重算輪廓。此修正不改三條既有增益公式或設定 token，但會修正舊版線性／平方模式的音色補償量。
11. **UI 不假冒 runtime 遙測。** 「APO 跟隨目標」由本列設定與來源快照計算，不代表 APO 已載入或量測輸出；不含 EQ、headroom 或其他衰減。來源失聯必須顯示未知，不能以 Editor 的最後讀值推定另一個 runtime 保留的增益。Studio 明示使用開窗時快照。

## 未採用方案

### 直接寫回 Windows 主音量

這會把讀取型 DSP 變成系統控制器，可能形成通知迴圈、和使用者或其他應用程式競爭，仍無法保證 Matrix 會套用該值，因此不採用。

2026-09-07 再評估「固定 Windows 100%、APO 全權控制」：手動 `Volume`＋`VolumeFollow Windows` 已能控制 APO dB，但不應自動拉高端點。`State 0`、停用 APO 或繞過處理路徑都會移除 APO 衰減；現有架構沒有全路徑執行確認與獨立 fail-safe 主音量層。硬體／軟體音量能力查詢只能辨識端點能力，無法證明每條串流實際經過此 APO。故維持 read-only；未來接管需獨立控制器、處理確認、端點切換／重開機復原與不受 EQ bypass 影響的增益護欄。[Microsoft endpoint volume hardware support](https://learn.microsoft.com/en-us/windows/win32/api/endpointvolume/nf-endpointvolume-iaudioendpointvolume-queryhardwaresupport)。

### 把跟隨增益併入 correction branch

共同 A 域不會被降低，`Attenuation 0` 也會意外關閉音量控制；低頻與高頻可能得到不同的寬頻音量語意，因此不採用。

### 只提供一條「模擬 Windows」曲線

Windows 的 audio-tapered scalar 不是穩定公開的固定公式，而且端點 dB 範圍也可由裝置決定。保留三個名稱與數學定義明確的選項，並讓 `Windows` 直接採用端點回報 dB。

### 讀取失敗就回 unity

在 Matrix 未自行衰減的情境會造成突然爆音。冷啟動先靜音、執行中保留最後有效衰減，比猜測 unity 安全。

## 後果與護欄

- 正常會自行套用 Windows 音量的端點不需改設定；`Off` 與舊設定完全相容。
- 使用者必須依實際路由選曲線，曲線名稱不是聲學校準或跨裝置一致性的承諾。
- 原生測試必須涵蓋 parser round-trip、缺省與重複欄位、三條公式、manual／automatic、mute、冷啟動失敗、執行中失敗與恢復、`State 0`、`Attenuation 0`、Full／Fast、立體聲與 in-place／out-of-place ramp。
- endpoint dB、scalar、mute 或來源可用性任一改變，都必須讓 runtime 與分析快照同步失效或更新。
- 未來若新增曲線，必須先定義設定相容、數學公式、邊界、mute、錯誤策略及即時測試，不能只在 UI 增加選項。
