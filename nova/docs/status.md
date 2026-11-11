# 進み具合

## M0 準備と試用（2026-11-12 開始）

- [x] ソース取得：`nowheel/RackDroid` v0.1.3.9（+1 コミット、e2f677fb）を clone、`nova` ブランチ作成
- [x] ビルド環境：cmdline-tools（Homebrew cask）、NDK 27.2.12479018、CMake 3.22.1、platforms;android-35、build-tools;34.0.0
- [x] 公式 APK とパックを取得（`nova/downloads/`）：arm64 APK、AudibleInstruments、Valley、JW-Modules
- [x] 無改造ビルドが Mac で通ることを確認（`assembleSideloadRelease -PdevKeystore -PtestBuild`、2 分 26 秒、APK 27.7 MB）
- [x] Nova の USB デバッグを有効にして接続（adb で認識）
- [ ] 公式版を入れてタッチで試用（見やすさ、音の遅れ）
- [x] 実機の情報を採る → `device-nova.md`（十字キーは HAT 軸、L2/R2 は BRAKE/GAS、遅延はおよそ 50 ms、Exclusive は拒否）
- [x] 公式 0.1.3.9 と 3 パックを導入（パックは読み込み済みをログで確認）
- [ ] 判断：これを土台にしてよいか
- [x] 公式版をタッチで試用（2026-11-12 のユーザーの感想）
  - 本体のキーは何も効かない（想定どおり。これから作る部分）
  - 画面を動かそうとするとモジュールが動いたり、つまみに触れたりして使いにくい（画面が小さいため）→ ボタン操作を優先する根拠
  - 音は鳴る
  - 既定のパッチでは鍵盤を押しても鳴らない → 既定のパッチに MIDI→CV が無いため。`Welcome.vcv` なら鳴る
