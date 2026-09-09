# ADR-0008：可設定的訊號鏈精度

狀態：已實作，尚未發布。

## 決策

`ProcessingPrecision: 32` 選擇整條設定的單精度訊號緩衝；`ProcessingPrecision: 64` 保留既有雙精度路徑。省略時為 64。同一份展開後的有效設定最多出現一次，包含 `Include` 檔案；未知值、額外 token 或重複宣告拒絕新設定，現有 active configuration 不變。Device、If 與 Stage 等控制 factory 先決定該行是否有效，再處理精度宣告。

每個 configuration 持有不可變的精度選擇。載入時配置 float 雙 bank、指標陣列及必要轉換空間；callback 只操作已配置記憶體。既有 FilterInfo 以 null mapping 表示沿用上一個路由，單精度 configuration 在發布前展開完整 mapping，並保留非 in-place filter 後的路由交換語意。

`IFilter::processSingle` 是可選的原生 float 核心；回傳 false 時不得改動緩衝或 filter state。此時 configuration 透過既有 double scratch 呼叫原核心，並把輸出存回 float bank。因此每個元件的音訊邊界是單精度，但係數設計、校準、FFT、外掛及特殊濾波器的內部計算可能仍為雙精度。原生 float 前級使用 float 乘數及運算。此選擇不保證降低 CPU 或記憶體用量，也不改變 Windows 裝置的輸出位元深度。

不同精度的 configuration 使用既有 pending／active／retired 交接。交叉淡化保留 double 輸出邊界，完成後發布新 configuration；回收仍由非即時路徑處理。離線分析讀取相同指令，不另外使用 UI 偏好覆蓋音訊語意。

## 編輯器

編輯器頂端標頭的「雙精度」開關（並與「設定」選單動作同步）修改目前設定檔，遵守既有即時儲存、手動儲存與復原流程。關閉開關寫入 `ProcessingPrecision: 32`，開啟開關寫入 `ProcessingPrecision: 64`；沒有 precision 宣告時預設勾選 64 位元。

含 `Include`、`Device`、`If`、`Stage` 或動態運算等作用域的檔案，編輯器不從單一分頁猜測有效值，也不自動加入可能重複的宣告；標頭開關與選單均停用，並提示以設定文字編輯精度。引擎仍支援這些設定的有效精度宣告。後續若擴充此控制，必須共用有效設定解析及來源定位，不能為更新 UI 狀態而載入外掛。

## 驗證

原生測試比較兩次前級乘法的 float 與 double 結果，並檢查 Copy → Preamp、Preamp → BiQuad → BiQuad、單精度空鏈、無效／重複精度重新載入，以及單精度到雙精度交叉淡化。這些案例涵蓋壓縮路由、非 in-place bank 交換及原生／轉換核心混用。
