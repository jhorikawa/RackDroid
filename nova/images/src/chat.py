"""How the chat is put together, for nova/README.md.  Run:  python3 nova/images/src/chat.py"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch

plt.rcParams["font.family"] = "Hiragino Sans"
INK = "#1c1813"
ASK, DO = "#2f6fb3", "#b5651d"   # 依頼と返事 / パッチの操作

fig, ax = plt.subplots(figsize=(16, 7.2), dpi=100)
ax.set_xlim(0, 16)
ax.set_ylim(0, 7.2)
ax.axis("off")
fig.patch.set_facecolor("white")


def box(x, y, w, h, title, lines, fc, ec=INK, lw=2, title_size=17):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.02,rounding_size=0.18", fc=fc, ec=ec, lw=lw))
    ax.text(x + w / 2, y + h - 0.42, title, ha="center", va="center", fontsize=title_size, weight="bold", color=INK)
    for i, t in enumerate(lines):
        ax.text(x + w / 2, y + h - 0.98 - i * 0.44, t, ha="center", va="center", fontsize=13, color=INK)


def arrow(x1, y1, x2, y2, color, text, tx, ty, size=12.5):
    ax.annotate("", xy=(x2, y2), xytext=(x1, y1),
                arrowprops=dict(arrowstyle="-|>", color=color, lw=2.6, mutation_scale=22))
    ax.text(tx, ty, text, ha="center", va="center", fontsize=size, color=color, weight="bold")


ax.add_patch(FancyBboxPatch((0.2, 0.75), 12.2, 6.2, boxstyle="round,pad=0.02,rounding_size=0.25", fc="#f6f2ea", ec="#8a8172", lw=2))
ax.text(0.55, 6.55, "Nova", fontsize=18, weight="bold", color=INK, va="center")
ax.add_patch(FancyBboxPatch((5.75, 1.05), 6.35, 5.2, boxstyle="round,pad=0.02,rounding_size=0.2", fc="#ebe5d8", ec="#8a8172", lw=1.6, ls="--"))
ax.text(8.92, 5.88, "Termux の中の Debian", fontsize=14, weight="bold", color="#5a5246", ha="center", va="center")

box(0.55, 1.4, 3.1, 4.4, "RackDroid", ["チャット画面", "（打つ・見る・止める）", "", "パッチを読む・変える", "（Undo は 1 依頼で 1 回）"], "#ffffff")
box(6.0, 1.4, 2.55, 4.1, "橋渡し", ["nova_bridge.py", "", "渡す道具は", "パッチの操作だけ"], "#ffffff")
box(9.35, 1.4, 2.5, 4.1, "Claude Code", ["公式のまま", "", "ログインは", "自分で行う"], "#ffffff")
box(13.15, 2.75, 2.6, 1.7, "Anthropic", ["Max の使用枠", "（インターネット）"], "#eef3f8")

arrow(3.75, 4.75, 5.92, 4.75, ASK, "依頼の文", 4.7, 5.05)
arrow(5.92, 3.75, 3.75, 3.75, ASK, "返事", 4.82, 4.03)
arrow(5.92, 2.5, 3.75, 2.5, DO, "追加・配線・つまみ", 4.7, 2.8, 11)
arrow(3.75, 1.75, 5.92, 1.75, DO, "いまのパッチ", 4.72, 2.03)
arrow(8.62, 4.05, 9.28, 4.05, ASK, "", 0, 0)
arrow(9.28, 3.65, 8.62, 3.65, ASK, "", 0, 0)
arrow(9.28, 2.4, 8.62, 2.4, DO, "", 0, 0)
arrow(8.62, 2.0, 9.28, 2.0, DO, "", 0, 0)
ax.annotate("", xy=(13.08, 3.6), xytext=(11.92, 3.6), arrowprops=dict(arrowstyle="<|-|>", color=INK, lw=2.6, mutation_scale=22))
ax.text(6.3, 0.36, "RackDroid と橋渡しの通信は Nova の中だけ。つながるたびに、合言葉を知っていることを互いに確かめる",
        ha="center", va="center", fontsize=13.5, color=INK)
ax.text(13.15, 1.5, "青  文のやり取り", fontsize=13, color=ASK, weight="bold", va="center")
ax.text(13.15, 1.08, "橙  パッチの操作", fontsize=13, color=DO, weight="bold", va="center")

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "README-chat-how.png")
fig.savefig(out, bbox_inches="tight", pad_inches=0.15)
print(os.path.normpath(out))
