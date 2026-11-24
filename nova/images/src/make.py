"""What a module made from the chat goes through, for nova/README.md.  Run:  python3 nova/images/src/make.py"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch

plt.rcParams["font.family"] = "Hiragino Sans"
INK = "#1c1813"
OK, BAD = "#3b7a3b", "#b3382f"

fig, ax = plt.subplots(figsize=(16, 8.2), dpi=100)
ax.set_xlim(0, 16)
ax.set_ylim(0, 8.2)
ax.axis("off")
fig.patch.set_facecolor("white")


def box(x, y, w, h, title, lines, fc, ec=INK):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.02,rounding_size=0.18", fc=fc, ec=ec, lw=2))
    ax.text(x + w / 2, y + h - 0.40, title, ha="center", va="center", fontsize=16, weight="bold", color=INK)
    for i, t in enumerate(lines):
        ax.text(x + w / 2, y + h - 0.92 - i * 0.40, t, ha="center", va="center", fontsize=12.5, color=INK)


def arrow(x1, y1, x2, y2, color, text="", tx=0, ty=0, style="-|>", rad=0.0):
    ax.annotate("", xy=(x2, y2), xytext=(x1, y1),
                arrowprops=dict(arrowstyle=style, color=color, lw=2.6, mutation_scale=22, connectionstyle=f"arc3,rad={rad}"))
    if text:
        ax.text(tx, ty, text, ha="center", va="center", fontsize=13, color=color, weight="bold")


ax.text(0.3, 7.85, "入れる前（Termux の中）", fontsize=17, weight="bold", color=INK, va="center")
box(0.3, 4.9, 2.9, 2.4, "書く", ["Claude が", "1 つのクラスを書く", "", ""], "#eef3f8")
box(4.1, 4.9, 3.2, 2.4, "コードを見る", ["ファイル・通信・生ポインタ", "などを使っていたら断る", "", "使う関数も一覧と照合"], "#ffffff")
box(8.2, 4.9, 3.2, 2.4, "コンパイル", ["配列の添字は毎回検査", "（動かしている間も残る）", "", "約 3 秒"], "#ffffff")
box(12.3, 4.9, 3.4, 2.4, "試運転", ["隔離した別プロセスで動かす", "端から端までの入力とつまみ", "2 分ぶんの早回し、速さ", "約 1.5 秒"], "#ffffff")
arrow(3.2, 6.1, 4.1, 6.1, OK)
arrow(7.3, 6.1, 8.2, 6.1, OK)
arrow(11.4, 6.1, 12.3, 6.1, OK)
ax.plot([14.0, 14.0, 1.75, 1.75], [4.9, 4.35, 4.35, 4.72], color=BAD, lw=2.6)
ax.plot([5.7, 5.7], [4.9, 4.35], color=BAD, lw=2.6)
ax.plot([9.8, 9.8], [4.9, 4.35], color=BAD, lw=2.6)
arrow(1.75, 4.5, 1.75, 4.9, BAD)
ax.text(7.75, 4.08, "だめなら、理由と行番号を Claude に返して書き直す（パッチは何も変わらない）", fontsize=13.5, color=BAD, weight="bold", ha="center", va="center")

ax.text(5.75, 3.35, "入れたあと（RackDroid の中）", fontsize=17, weight="bold", color=INK, va="center")
box(0.3, 0.45, 4.5, 2.5, "差し替え", ["再起動なしで、動いたまま入れ替え", "ケーブルとつまみはそのまま", "", "前の版に戻せる（6 版まで）"], "#f1f7ef", OK)
box(5.75, 0.45, 4.5, 2.5, "見張り", ["不正なメモリ、添字の範囲外、例外、", "終わらないループ、数でない出力", "", "→ そのモジュールだけ止める"], "#fff5e6", "#b5651d")
box(11.2, 0.45, 4.5, 2.5, "落ちてしまったら", ["原因のモジュールを覚えておく", "", "次の起動では代役が立ち、", "ケーブルもつまみも残る"], "#fbecea", BAD)
arrow(4.8, 1.7, 5.75, 1.7, INK)
arrow(10.25, 1.7, 11.2, 1.7, INK)
ax.plot([14.6, 14.6, 2.55], [4.9, 3.72, 3.72], color=OK, lw=2.6)
arrow(2.55, 3.72, 2.55, 2.98, OK, "合格", 13.95, 3.5)

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "README-make-how.png")
fig.savefig(out, bbox_inches="tight", pad_inches=0.15)
