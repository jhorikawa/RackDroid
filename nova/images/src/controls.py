"""Button map for nova/README.md.  Run:  python3 nova/images/src/controls.py"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Circle, FancyBboxPatch, Rectangle

plt.rcParams["font.family"] = "Hiragino Sans"
INK, PANEL, BODY, LINE = "#1c1813", "#2b2721", "#3a342c", "#8a8172"
VIEW, EDIT, PLAY = "#2f6fb3", "#b5651d", "#3d8b37"   # 画面の移動 / 操作 / 演奏

fig, ax = plt.subplots(figsize=(16, 9), dpi=100)
ax.set_xlim(0, 16)
ax.set_ylim(0, 9)
ax.axis("off")
fig.patch.set_facecolor("white")

# 本体
ax.add_patch(FancyBboxPatch((4.2, 2.6), 7.6, 3.9, boxstyle="round,pad=0.05,rounding_size=0.5", fc=BODY, ec=INK, lw=2))
ax.add_patch(Rectangle((6.3, 3.25), 3.4, 2.6, fc="#0f0d0a", ec=LINE, lw=1.5))
ax.text(8.0, 4.55, "画面\n（タッチも使える）", color="#d8cfbd", ha="center", va="center", fontsize=13)
# 肩のボタン
for x, name in ((4.5, "L2"), (5.45, "L1"), (9.65, "R1"), (10.6, "R2")):
    ax.add_patch(FancyBboxPatch((x, 6.6), 0.85, 0.34, boxstyle="round,pad=0.02,rounding_size=0.1", fc=PANEL, ec=INK, lw=1.5))
    ax.text(x + 0.425, 6.77, name, color="white", ha="center", va="center", fontsize=11, weight="bold")
# 左：スティックと十字キー
ax.add_patch(Circle((5.2, 5.35), 0.42, fc=PANEL, ec=INK, lw=1.5))
ax.add_patch(Circle((5.2, 5.35), 0.22, fc="#55504a", ec=INK))
for dx, dy in ((0, 0.3), (0, -0.3), (0.3, 0), (-0.3, 0), (0, 0)):
    ax.add_patch(Rectangle((5.2 + dx - 0.15, 3.75 + dy - 0.15), 0.3, 0.3, fc=PANEL, ec=INK, lw=1))
# 右：ABXY とスティック
for dx, dy, name in ((0.36, 0, "A"), (0, -0.36, "B"), (0, 0.36, "X"), (-0.36, 0, "Y")):
    ax.add_patch(Circle((10.8 + dx, 5.35 + dy), 0.17, fc=PANEL, ec=INK, lw=1.2))
    ax.text(10.8 + dx, 5.35 + dy, name, color="white", ha="center", va="center", fontsize=10, weight="bold")
ax.add_patch(Circle((10.8, 3.75), 0.42, fc=PANEL, ec=INK, lw=1.5))
ax.add_patch(Circle((10.8, 3.75), 0.22, fc="#55504a", ec=INK))
# Select / Start
for x, name in ((7.1, "Select"), (8.3, "Start")):
    ax.add_patch(FancyBboxPatch((x, 2.78), 0.62, 0.22, boxstyle="round,pad=0.02,rounding_size=0.08", fc=PANEL, ec=INK, lw=1))
    ax.text(x + 0.31, 2.89, name, color="white", ha="center", va="center", fontsize=8)


def line(x, y, tx, ty, color):
    ax.annotate("", xy=(tx, ty), xytext=(x, y), arrowprops=dict(arrowstyle="-", color=color, lw=1.6))


def note(x, y, title, body, color, tx, ty, ha):
    """説明の箱と、ボタンへの引き出し線。"""
    line(x, y, tx, ty, color)
    ax.text(x, y, f"{title}\n{body}", ha=ha, va="center", fontsize=11.5, color=INK, linespacing=1.5,
            bbox=dict(boxstyle="round,pad=0.45", fc="white", ec=color, lw=2))


note(3.55, 7.3, "L2 / R2", "ズーム（L2 で縮小、R2 で拡大）", VIEW, 4.6, 6.96, "right")
# L1 と R1 は 1 つの説明から両方へ線を引く（L2 の上を横切らせない）
line(8.0, 7.75, 10.07, 6.96, EDIT)
note(8.0, 7.75, "L1 / R1", "短く押す：前 / 次のモジュールへ　　押したまま十字キー：モジュールを運ぶ", EDIT, 5.87, 6.96, "center")
note(3.55, 5.35, "左スティック", "画面の移動（パン）\n押し込み：演奏モード中はゲート", VIEW, 4.78, 5.35, "right")
note(3.55, 3.75, "十字キー", "フォーカスを隣のつまみ・ジャックへ\n編集中：↑↓ 粗く、←→ 細かく", EDIT, 4.75, 3.75, "right")
# A / B / X / Y は 1 つにまとめる（1 つずつ線を引くと、線がほかのボタンの上を通る）
note(12.45, 5.75, "A / B / X / Y",
     "A：つまみを編集 ／ スイッチを切替\n　　ジャックからケーブルを始める・つなぐ\n"
     "B：戻る（編集終了、配線取消、\n　　つまみ → モジュール選択 → 枠を消す）\n"
     "X：モジュールを追加\n"
     "Y：消去（つまみを初期値に、ケーブルを抜く）\n"
     "モジュール選択中は\n　　A：つまみに入る　A＋十字キー：運ぶ\n　　Y 長押し：モジュールを消す", EDIT, 11.4, 5.35, "left")
note(12.45, 2.6, "右スティック", "フォーカス中のつまみを回す\n演奏モード：CV（左右・上下）\n押し込み：演奏モードの切替", PLAY, 11.24, 3.75, "left")
note(6.0, 1.55, "Select", "一時停止 / 再生　　長押し：ズームの切替", EDIT, 7.41, 2.76, "center")
note(10.0, 1.55, "Start", "メインメニュー", EDIT, 8.61, 2.76, "center")

for i, (c, t) in enumerate(((VIEW, "画面の移動"), (EDIT, "操作"), (PLAY, "演奏（CV）"))):
    ax.add_patch(Rectangle((5.6 + i * 2.1, 8.47), 0.26, 0.26, fc=c, ec="none"))
    ax.text(5.95 + i * 2.1, 8.6, t, va="center", fontsize=12, color=INK)
ax.text(8.0, 0.7, "演奏モードでは本体の傾きも CV になる。変わるのは右スティックだけで、ほかのボタンはそのまま使える。",
        ha="center", va="center", fontsize=12, color=INK)

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "README-controls.png")
fig.savefig(out, facecolor="white", bbox_inches="tight", pad_inches=0.25)
print(os.path.normpath(out))
