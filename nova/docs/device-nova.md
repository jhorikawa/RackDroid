# Retroid Pocket Nova の実測メモ（2026-11-12、adb で取得）

| 項目 | 値 |
|---|---|
| 機種 / ビルド | Retroid Pocket Nova（device `kalama`）/ `RPN_V1.0.0.436_20260722_083524_user` |
| OS | Android 13（API 33）、SoC QCS8550、ページサイズ 4096 |
| 画面 | 1280×960（物理は 960×1280 を回転）、密度 320（pixelRatio 2.0 → Rack のシーンは 640×480 単位） |
| 機能フラグ | `audio.low_latency`、`audio.pro`、`software.midi`、`usb.host`、`sensor.gyroscope`、`sensor.accelerometer`、`bluetooth_le` すべてあり |
| AAudio の設定 | `aaudio.mmap_policy=2`、`aaudio.mmap_exclusive_policy=2`、`aaudio.hw_burst_min_usec=2000` |

## コントローラー（`dumpsys input`）

- デバイス名 **Retroid Pocket Controller**、Sources は `KEYBOARD | GAMEPAD | JOYSTICK`、バイブあり
- 軸：`X` `Y`（左スティック）、`Z` `RZ`（右スティック）、`BRAKE` `GAS`（L2 / R2、0〜1 のアナログ）、`HAT_X` `HAT_Y`（**十字キーはキーではなく HAT 軸**）
- スティックの flat（遊び）は 0.00046 とほぼゼロ。デッドゾーンは自前で入れる
- ほかに **Retroid Pocket Virtual Mouse**（Sources `MOUSE`、0〜1279 × 0〜959）がある。本体のキーマップ機能用とみられる
- ボタンのキーコードは未実測（M2 で `getevent` と実機操作で採る）

## 公式 RackDroid 0.1.3.9 のオーディオ（logcat）

- `sampleRate=48000 block=512 callback=576 burst=192 sharing=Shared performance=LowLatency api=AAudio`
- **Exclusive は拒否された**（`device MMAP support=1, this stream using MMAP=0`）。HAL より上の層が断っている
- 実測のストリーム遅延 **38.9 ms**、エンジンの 512 フレームのブロックでさらに **10.7 ms**（合計およそ 50 ms）
- オーディオのコールバックは SCHED_FIFO。cpu7（最速コア）には固定できず cpu5 に落ち着いた
- パックの読み込み：3 パックで 53 ms。場所は `/sdcard/Android/data/org.rackdroid/files/Modules/`（adb push で置ける）
