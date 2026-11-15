# Project Sync & Release Policy (專案全自動雲端建置與同步規範)

對本專案進行**任何修改**時，一律採用 **GitHub Actions 全自動打包與雙重發佈管線**（徹底免除本機 OAuth 1 小時過期重連限制）：

1. **開發端唯一動作：將原始碼推送至 GitHub (`main` 分支)**
   - 所有 Kotlin、NDK C++20、CMake、Gradle 組態與測試原始碼變更，提交並推送至：`https://github.com/Sinoro-Code/homography-auto-mosaic`。
   - **無須**在本機呼叫過期受限之雲端硬碟 OAuth 插件，亦**嚴禁**將原始碼上傳至 Google 雲端硬碟。

2. **GitHub Actions 自動觸發（快取命中後約 30~45 秒完成）**
   - 工作流程 `.github/workflows/build-and-sync-apk.yml` 於每次 `push` 至 `main`（或手動 `workflow_dispatch`）時自動執行：
     1. 還原預編譯 NDK 靜態庫快取（`OpenCV 4.10.0` + `libjpeg-turbo 3.0.4`）與 Gradle / CMake 快取，增量編譯並以一致金鑰簽章產出 `自碼系統.apk`。
     2. **自動發佈至 GitHub Releases 固定網址**（`latest` 標籤，顯示名稱 `自碼系統.apk`，手機可直接下載）：
        - `https://github.com/Sinoro-Code/homography-auto-mosaic/releases/latest/download/AutoMosaic.apk`
     3. **自動原地覆寫至 Google 雲端硬碟「`自碼系統`」資料夾內之 `自碼系統.apk`**（透過 `scripts/ci_sync_gdrive.py` 與永久不過期之 Google Cloud Service Account 金鑰 `GDRIVE_SERVICE_ACCOUNT_JSON` 執行 `PATCH` 原地更新，保留固定檔案連結且免配額限制）。
