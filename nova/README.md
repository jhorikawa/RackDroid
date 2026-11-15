# RackDroid for Retroid Pocket Nova

[RackDroid](https://github.com/nowheel/RackDroid)（VCV Rack 2 の Android 移植）を、ゲームパッド付きの携帯機 **Retroid Pocket Nova** で、画面に触れずに使えるようにした fork です。

本家はタッチ専用です。4.5 インチの画面では、画面を動かすつもりの指がモジュールやつまみを動かしてしまいます。この fork は、十字キーでつまみやジャックを選び、ボタンで編集・配線・モジュールの追加と移動ができるようにしました。スティック・本体の傾き・タッチパッドを、パッチの中の電圧（CV）として使うこともできます。

タッチ操作など本家の機能はそのまま残してあります。パッチは VCV Rack 2 と同じ `.vcv` です。

- 本家：`nowheel/RackDroid`（GPLv3）。この fork も GPLv3 です。本家・VCV とは無関係の非公式な改造です。
- 開発の記録：[docs/status.md](docs/status.md)、計画：[docs/plan.md](docs/plan.md)

## 操作

![Nova のボタンと、それぞれの役割を示した図](images/README-controls.png)

ボタンの割り当て。青は画面の移動、茶は操作、緑は演奏（CV）。

十字キーを押すと、いちばん近いつまみかジャックに**フォーカスの輪**が付きます。画面下には「モジュール名 - つまみ名　値」が出ます。フォーカス中のつまみが属するモジュールには、薄い枠が付きます。

![VCA のつまみにフォーカスの輪が付き、VCA 全体に薄い枠が出ている画面](images/README-focus.png)

フォーカスの輪（つまみ）と、そのモジュールを示す薄い枠。画面下に名前と値。

| ボタン | フォーカスが… | 動作 |
|---|---|---|
| 十字キー | — | 近くのつまみ・ジャックへ移動。モジュールの端では隣のモジュールへ |
| A | つまみ | 編集モード。↑↓ で粗く、←→ で細かく。A か B で終了 |
| A | スイッチ・ボタン | 切り替え。押している間だけ効くボタンは、A を押している間だけオン |
| A | ジャック | ケーブルを始める。つなげられるジャックだけを移動でき、行き先で A を押すと接続 |
| A | つまみのないモジュール | そのモジュールを持ち上げる（移動） |
| B | — | 戻る。編集の終了、配線の取消、どちらでもなければフォーカスを消す |
| Y | つまみ / ジャック | つまみを初期値に戻す / ジャックのケーブルを抜く |
| X | — | モジュールを追加 |
| 右スティック | つまみ | 直接回す（上下で大きく、左右で細かく） |
| 左スティック | — | 画面の移動（パン） |
| L2 / R2 | — | 縮小 / 拡大 |
| Select | — | ズームの切替（等倍 → 2 倍 → 全体） |
| Start | — | メインメニュー |

値の変更、配線、モジュールの移動は、Undo で取り消せます。フォーカスを消したあとは、B 以外のボタンを押すと同じ場所に戻ります（最初の一押しは戻すだけです）。

## メニュー

Start でメインメニュー、X でモジュールの追加が開きます。十字キーで選び、A で決定、B で戻る、L1 / R1 でページ送りです。

![メインメニュー。各行にアイコンが付き、3 行目が選ばれている](images/README-menu.png)

メインメニュー。ツールバーやモジュールパレットはタッチ専用なので、同じ機能へここから行けるようにしてあります。

| 項目 | 内容 |
|---|---|
| Add module… | 種類（VCO、LFO、VCF …）を選び、一覧からモジュールを選ぶ |
| Move module | フォーカス中のモジュールを移動する |
| Module menu… | フォーカス中のモジュールのメニュー（初期化、複製、削除、バイパス、プリセット） |
| XY pad | タッチパッドを出す・しまう |
| Undo / Redo | 取り消し / やり直し |
| File… / Edit… / View… / Engine… / Help… | 本家のメニュー（保存、開く、表示設定など） |
| Tools… | ツールバーのアイコンと同じ機能（鍵盤、録音、MIDI、ロックなど） |

## モジュールを動かす

**L1 か R1 を押したまま十字キー**を押すと、フォーカス中のモジュールが動きます。離すと確定です。

![L1 を押したままモジュールを動かしている画面。モジュールが緑の枠で囲まれている](images/README-move.png)

移動中のモジュールは緑の枠で囲まれます。

- 左右：隣のモジュールと場所を入れ替える。隣が空いていれば、その方向へ進む
- 上下：上・下の段へ移す。移った先のモジュールが場所を空ける

X で追加した直後のモジュールは、最初から移動モードになっています。十字キーで置き場所を決めて A で確定、B で元の場所に戻します。メインメニューの「Move module」からも同じモードに入れます。

## 演奏：スティック・傾き・タッチパッドを CV に

VCV Rack 標準の「Gamepad」MIDI ドライバとして見えるので、専用のモジュールは要りません。**MIDI CC to CV** モジュールを置き、入力元に「Gamepad」→「Built-in controller」を選び、出力に下の番号を割り当てて、ケーブルでつなぎます。

| CC | 入力 | 電圧 | 使える条件 |
|---|---|---|---|
| 2 / 3 | 右スティックの左右 / 上下 | ±10 V（右・上が正） | 演奏モード |
| 15 | L3（左スティックの押し込み） | 0 / 10 V | 演奏モード |
| 21 / 22 | 本体の傾き：左右 / 前後 | ±10 V（右を下げる・奥へ倒すが正。35 度で最大） | 演奏モード |
| 23 / 24 | タッチパッドの X / Y | 0〜10 V（左→右、下→上） | パッドが出ている間 |
| 25 | タッチパッドに触れている | 0 / 10 V | パッドが出ている間 |

**演奏モード**は R3（右スティックの押し込み）で切り替えます。画面右上に緑の「PLAY」が出ます。変わるのは右スティックだけで、左スティックのパン、L2 / R2 のズーム、十字キーと A / B での操作はそのまま使えます。演奏中につまみを動かすときは、A を押して十字キーで調整します。

傾きは、演奏モードに入ったときの持ち方が中心です。取り直すときは R3 を 2 回押します。

![画面の下半分に XY パッドが出ていて、指の位置に点と十字の線がある](images/README-xy-pad.png)

タッチパッド（Start →「XY pad」）。指を離すとゲートが切れ、X と Y は最後の位置を保ちます。

デモパッチが入っています（Start → File… → Open）。

| パッチ | 試せること |
|---|---|
| Nova 1 - Stick CV | R3 で演奏モードにして右スティックを倒す。左右でフィルター、上下で音程 |
| Nova 2 - Tilt CV | R3 で演奏モードにして本体を傾ける。左右でフィルター、前後で音程 |
| Nova 3 - XY Pad | タッチパッドに触れている間だけ音が出る。左右で音程、上下でフィルター |

## Mac とパッチをやり取りする

パッチは Nova の `/sdcard/Android/data/org.rackdroid.test/files/Patches/` に入ります（本家はアプリの中にしまっていて、外から読めません）。USB でつなぎ、`tools/novasync` で送受信します。

```sh
nova/tools/novasync list                       # Nova にあるパッチ
nova/tools/novasync push "My Patch.vcv"        # Mac → Nova（すぐ File > Open に出る）
nova/tools/novasync pull "My Patch.vcv" out/   # Nova → Mac
nova/tools/novasync pull                       # 全部を ./patches-from-nova へ
nova/tools/novasync recordings                 # 録音した WAV を取り出す
```

Nova で保存したパッチは、デスクトップ版の VCV Rack 2 と同じ形式です。開く側に同じモジュール（プラグイン）が入っていれば、そのまま開けます。

## ビルドと導入

Mac での手順です。Android Studio（同梱の JDK 21 を使う）と、Android SDK の NDK 27.2.12479018・CMake 3.22.1・API 35 が要ります。

```sh
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
export ANDROID_HOME="$HOME/Library/Android/sdk"
./gradlew assembleSideloadRelease -PdevKeystore -PtestBuild
adb install -r app/build/outputs/apk/sideload/release/app-sideload-release.apk
```

`-PtestBuild` を付けると、アプリ ID が `org.rackdroid.test`（表示名「RackDroid TEST」）になり、公式版と並べて入ります。モジュールパック（`.rdmod`、本家のリリースにあります）は `/sdcard/Android/data/org.rackdroid.test/files/Modules/` に置いて再起動します。

## わかっていること・まだのこと

- Nova（Android 13）でだけ確かめています。ほかのゲームパッドは、十字キーがキーでも軸でも届くように作ってありますが、試していません。
- 音の遅れは Nova で合計およそ 50 ms です（本家と同じ。Nova が最速のオーディオ経路を許可しないため）。
- 傾きの向きと効き具合は、実機での調整がまだです。
- VCV Rack の Audible Instruments にない Mutable 系モジュール（Beads、Peaks、Edges、Blades）はありません。
- 改造の中心は `native/port/gamepad_*.cpp` と `app/src/main/java/org/rackdroid/{GamepadMenu,TiltSensor,XyPad}.kt` です。VCV Rack 本体（`third_party/`）には手を入れていません。
