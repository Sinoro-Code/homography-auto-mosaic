# Project Sync & Release Policy (專案同步與打包規範)

對本專案進行**任何修改**時，必須嚴格遵守以下雙軌同步規範：

1. **原始碼（Source Code）-> 僅同步至 GitHub**
   - 所有 Kotlin、NDK C++20、CMake、Gradle 組態與測試原始碼變更，一律提交並推送至 GitHub 儲存庫：`https://github.com/Sinoro-Code/homography-auto-mosaic`（`main` 分支）。
   - **嚴禁**將原始碼目錄或原始碼壓縮包上傳至 Google 雲端硬碟。

2. **打包產物（Packaged Artifacts，如 `自碼系統.apk` 等）-> 同步至 Google 雲端硬碟專屬資料夾**
   - 每次修改涉及應用程式功能、UI 或原生引擎時，須重新執行 Release 打包（`./gradlew :app:assembleRelease` 產出已簽章之 `app/build/outputs/apk/release/自碼系統.apk`），並將打包產物（`.apk` / `.aab` 等）同步至 Google 雲端硬碟之 **`自碼系統`** 專屬資料夾。
   - 一律透過 `./scripts/sync_github_and_gdrive.py` 執行冪等原地更新（`gdrive mutate update`），避免在雲端資料夾內產生重複檔案。
