# 第三方组件与许可 / Third-party components and licenses

本分支（SuperFW 中文增强版，"Long" 版本）是上游 [davidgfnet/superfw](https://github.com/davidgfnet/superfw) 的分支。
下面列出固件里和发布包里用到的所有外部内容、它们的许可，以及**我核对到了什么程度**。
"已核对"表示我在仓库文件里直接看到了许可文本；"按上游标注"表示这是上游 SuperFW 的说明，我没有独立核对；
"未确认"表示我没能找到明确的许可文本。

This fork ("Long" releases) is based on upstream SuperFW. This file lists the external content used by the
firmware and the release packages, its license, and how far that was verified.

## 本分支自己的部分

| 内容 | 许可 | 说明 |
|---|---|---|
| 本分支对 SuperFW 的修改（菜单、字体切换、主题、图标、设置分页、刷机选择等） | GPL-3.0-or-later，同上游 | 完整许可文本见 [LICENSE](LICENSE) |
| 平台图标（GG、SG-1000、Super Vision、NGP、NGC、PC Engine）和 16×12 图标 | GPL-3.0-or-later | 本分支自绘 |
| 10 个主题的配色 | GPL-3.0-or-later | 本分支设计 |
| `tools/acl2cht.py` | GPL-3.0-or-later | 本分支编写的格式转换工具 |

## 上游 SuperFW 及其自带的组件

| 组件 | 许可 | 来源 | 核对情况 |
|---|---|---|---|
| SuperFW（David Guillen Fandos） | GPL-3.0-or-later | <https://github.com/davidgfnet/superfw> | 已核对：源码文件头写明 GPL v3 或更高版本。注意上游仓库自己没有放 LICENSE 文件，本分支补上了 |
| FatFs R0.15（ChaN） | 类 BSD 的 1 条款许可 | `fatfs/` 目录 | 已核对：`fatfs/ff.c` 文件头 |
| nanoprintf | 公共领域 | 上游 | 按上游标注 |
| heapsort | 3 条款 BSD | 上游 | 按上游标注 |
| apultra、upkr（只在编译时使用，上游用 C++ 重新实现） | zlib / 公共领域 | `tools/` | 按上游标注 |
| 部分链接脚本 / crt0（AntonioND） | CC0 | 上游 | 按上游标注 |

## 字体

| 组件 | 许可 | 来源 | 核对情况 |
|---|---|---|---|
| **Fusion Pixel Font**（TakWolf）10px / 12px，用于汉字、假名、英文、拉丁、希腊、西里尔、韩文 | SIL Open Font License 1.1 | <https://github.com/TakWolf/fusion-pixel-font> | 已核对：许可文本在 [res/fonts/LICENSE-fusion-pixel-OFL.txt](res/fonts/LICENSE-fusion-pixel-OFL.txt)。该字体包含 Ark Pixel、Cubic 11、Galmuri 等字体的成分，授权见其发布包 |
| Unscii（viznut） | 按上游标注为 GPL | <http://viznut.fi/unscii/> | 按上游标注（`res/fonts/README.md`）。Fusion Pixel 没有的少数字符回退到它 |
| GNU Unifont（`hangul-blocks.hex` 的来源） | GPL，带字体嵌入例外 | <https://unifont.org/> | 按上游标注 |

字体数据被转换成固件使用的格式（`res/fonts*.pack`），原始字体的版权声明和许可文本随仓库一起提供。

## 内置的模拟器（SuperChis 版固件自带）

这 6 个模拟器是**上游 SuperFW 已经在用的同一批二进制文件**，本分支没有增删或改动
（和上游 `emu/` 目录逐字节一致）。来源记录在 [emu/README.md](emu/README.md)。

| 模拟器 | 平台 | 来源 | 许可核对情况 |
|---|---|---|---|
| Jagoomba Color v0.5 | GB / GBC | <https://github.com/EvilJagaGenius/jagoombacolor> | **未确认**：仓库里没有许可文件，README 说明它是 Goomba Color（2019-05-04 源码）的分支 |
| PocketNES（2013-07-01） | NES | <https://www.dwedit.org/gba/pocketnes.php> | **未确认**：仓库里没有找到许可文本 |
| WasabiGBA v0.2.4 | Super Vision | <https://github.com/FluBBaOfWard/WasabiGBA> | **未确认**：仓库根目录没有许可文件 |
| NGPGBA v0.5.7 | NGP / NGC | <https://github.com/FluBBaOfWard/NGPGBA> | **未确认**：仓库根目录没有许可文件 |
| PCE-Advance v7.5 | PC Engine | <https://github.com/patters-match/pceadvance> | **未确认**：仓库里没有许可文件。上游把其中 2 个字节改成 Supercard 需要的 WAITCNT 值 |
| SMSAdvance v2.5（`smsadvance-v2.5-scptch`） | SMS / GG / SG-1000 | **上游没有记录来源**（`emu/README.md` 里没有这一项） | **未确认** |

说明：
- GitHub 没有识别出这些仓库的许可，**不等于没有许可**，也可能是许可写在源码文件里。我没有逐个检查源码。
- 它们作为独立的 GBA 程序（压缩后放在固件的资源区）分发，不是编译进 SuperFW 代码里的，SuperFW 只是把它们加载运行。
- 如果你是这些模拟器的作者，认为这样分发不合适，请到 <https://github.com/kylecug/superfw/issues> 告诉我，我会处理；也可以直接联系上游。

## 汉化金手指（只在 Release 的压缩包里，不在源码仓库里）

| 内容 | 来源 | 许可核对情况 |
|---|---|---|
| `superfw-cheats-zh-*.zip` 里的 2477 个 `.cht` 文件 | 用 `tools/acl2cht.py` 从 [open_agb_firm](https://github.com/profi200/open_agb_firm) 的金手指合集 `gba.acl` 转换而来 | **未确认**：open_agb_firm 仓库本身是 GPL-3.0，但这份金手指数据是社区收集整理的，原作者和各自的授权我无法核实 |

- 本分支只做了格式转换，权利归金手指原作者和整理者所有。
- 如果你是这些金手指的作者，不希望被收录，请到 <https://github.com/kylecug/superfw/issues> 告诉我，我会从发布包里移除。
- 想完全避开这个问题的话，可以使用不带金手指的固件包：只下载 `superfw-chis.fw`，金手指自己准备（官方库：<https://github.com/davidgfnet/superfw-cheats>，上游没有声明它的许可）。

## 源码

固件的完整源码就是这个仓库：<https://github.com/kylecug/superfw>。每个 Release 对应的源码是同名 tag
（例如 `v0.22-long10`）。按 GPL-3.0，你有权获得并修改、再分发它，条件见 [LICENSE](LICENSE)。
