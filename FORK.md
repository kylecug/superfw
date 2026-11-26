# SuperFW 中文增强分支（0.22.x）

这是 [davidgfnet/superfw](https://github.com/davidgfnet/superfw)（Supercard 的开源固件）的一个分支，
基于上游 **v0.22**，版本号显示为 **0.22 Long v7**（上游版本号不变，后面加的是本分支的名字 Long 和自己的版本号）。

主要目标：中文使用更顺手（字体、菜单、金手指匹配），并把菜单操作整理得更统一。
**目前只编译和使用 Supercard SuperChis 版本（`chis`）**，`sd` / `lite` 版本没有编译，也没有测试。

## 改动一览

### 菜单操作

- **三个游戏页面**：最近运行、浏览、NOR，顶部用文字标签显示（中文：最近 / 浏览 / NOR），当前页高亮。
  - 按 **L / R** 依次循环切换。
  - 页内的左右方向键是上一页 / 下一页翻页。
- **开机默认进入"最近运行"页**。"最近运行"选项已去掉，固定开启，列表为空时显示空提示。
- **START 键打开系统菜单**，右下角常驻提示"START/系统"。菜单是一个贴着右下角的小窗口，里面是
  设置、工具、信息三项，上下选择，A 进入，B 返回。
- **设置、工具、信息都是右下角弹出的窗口**，叠在当前页面上，不再全屏。
  - 原来的"设置"和"界面/语言"合并成一个设置窗口，**设置分成四组，顶部是标签**：
    界面（主题、语言、文字速度、字体大小、隐藏文件）、系统（快捷键、启动、快速 SD、NOR 校验、EWRAM、金手指）、
    存档（存档/状态位置、备份数、保存策略、优先 DS）、游戏（补丁、游戏内菜单、读取策略、RTC）。
    每组都不用滚动，**L / R 切换分组**，上下键到头会循环（第一行按上直接到"保存"）。
  - 方向键左右修改数值；底部是当前项的说明（过长会滚动），没有说明时显示 L/R 切换分组的提示。
  - 每组最后一行都是"保存"按钮，同时写入 `settings.txt` 和 `ui-settings.txt`。
  - 窗口里每行 14 像素（12 像素高的色条加 2 像素间隔），12px 字体也有间隔。
  - **记住上次的位置**：START 菜单记住上次选的项，设置窗口记住上次的分组和每组选中的行，工具窗口也记住（本次开机内有效）。
  - 信息页的固件更新解锁组合键（下 + B + START）保持不变，窗口里同样有效。
- 信息页显示 `Version 0.22 Long v7`。名字和版本号在 `Makefile` 的 `CUSTOM_NAME` 和 `CUSTOM_VERSION`，只用于显示，不影响固件更新的版本判断。

### 字体

- 汉字和英文改用 [Fusion Pixel Font](https://github.com/TakWolf/fusion-pixel-font) 点阵字体（SIL OFL 1.1），
  **12px 和 10px 两种大小都内置，在"设置"里的"字体大小"选择，重启后生效**（默认 12px）。
  - 12px：汉字 12×12，比原来的 16×16 窄 25%。文件列表行高 14 像素，一页 9 行（原来 8 行）。
  - 10px：汉字 10×10，窄 37.5%。文件列表行高 12 像素，一页 11 行，底栏缩到 12 像素。
    覆盖的汉字比 12px 少：GB2312 一级字（3755 个常用字）全部有，二级字约 87% 有，
    CJK 统一表意文字区共 10.7k 个（12px 版是 19.5k）。没有的字显示空心方框。
  - 12px 字库不压缩，开机时直接复制到内存；10px 字库用 apultra 压缩后作为资源（`FNT1`）放进固件，
    选了 10px 后开机时再解压覆盖。设置保存在 `ui-settings.txt` 的 `font_size`（0 = 12px，1 = 10px）。
  - **默认设置启动**：固件启动（出现 SuperFW 页面）时按住 **R** 键，本次完全不读取 SD 卡上的设置
    （`settings.txt` 和 `ui-settings.txt`），全部用默认值（12px 字体、英文、默认主题），屏幕下方会显示
    "R held: using default settings"。从 DS 上运行 GBA 卡带时也一样，按的是固件启动那一刻。
    万一某个设置导致无法启动，不用拔卡改文件。进设置保存一次，就会把默认值写回 SD 卡。
  - SuperChis 版固件里 ROM 资源区在 0x190000 之前，字库和模拟器都放在这里，所以第二套字库必须压缩。
- 字体包：`res/fonts-ext.pack`（12px）和 `res/fonts-ext-10px.pack`（10px）是 SuperChis 版实际使用的，多了预组合的韩文音节；
  `res/fonts.pack`（12px，Lite/SD 版）没有编译测试。**注意**：0.22.1 只更新了 `fonts.pack`，
  而 SuperChis 版编译时用的是 `fonts-ext.pack`（Makefile 里的 `FONTS_EXT`），所以 0.22.1 的 SuperChis 固件里
  汉字其实还是原来的 16 像素。0.22 Long v2 起 SuperChis 用的字库已更新。
- 英文（ASCII）字形现在放在字库里（跟随字体大小），固件内置的 12px 英文字库只作为后备。
- 文件列表的图标改成 12 行高（16×12），并新增了 Game Gear、SG-1000、Super Vision、Neo Geo Pocket、
  Neo Geo Pocket Color、PC Engine 六种平台的图标（原来显示为通用文件图标）。图标按扩展名识别。
- 带音标的拉丁字母（Latin-1 / 扩展 A、B）、希腊文、西里尔文、韩文也改用 Fusion Pixel（跟随字体大小）。
  拉丁、希腊、西里尔里 Fusion Pixel 没有、或比 8 像素更宽的少数字符，仍回退到原来的 16 像素字体。
  韩文音节 11172 个全部有，字库也因此变小（10px 版固件里压缩前 660 KB，12px 版 790 KB）。
- 字体生成工具见 `res/fonts/`：`fusion2hex.py`（BDF 转换，`--size 10|12`）和 `generator.py`（新增 12、10 像素宽块类型），
  命令写在 `res/fonts/README.md`。用旧参数重新生成仍能逐字节还原原来的字体文件。

### 金手指

- **匹配规则放宽**。`/.superfw/cheats/` 下，按下面的顺序找最合适的文件：
  1. 和 ROM 同名、放在 ROM 旁边的 `.cht`
  2. `XXXX-VV.cht`（游戏代码 + 版本号，和上游一致）
  3. `XXXX.cht` 或 `XXXX-任意版本.cht`（忽略版本号）
  4. 游戏代码前三位相同的文件（汉化版常常改了第 4 位语言字母或版本字节）
- 修复了一个上游遗留问题：和 ROM 同名的 `.cht` 即使找到了也不会被载入。
- `tools/acl2cht.py`：把 [open_agb_firm](https://github.com/profi200/open_agb_firm)
  的金手指数据库 `gba.acl` 转换成 SuperFW 的 `.cht` 文件。原数据库是"每帧往 RAM 写固定字节"的格式，
  可以直接转成 CodeBreaker 的 8 位 / 16 位写入码（`3AAAAAAA+00VV`、`8AAAAAAA+VVVV`）。

  ```
  python tools/acl2cht.py gba.acl 输出目录
  ```

  把输出目录里的文件复制到 SD 卡的 `/.superfw/cheats/` 即可。**转换好的结果已经放在 Release 里**
  （`superfw-cheats-zh-v0.22.1.zip`，2477 个文件，直接解压到 SD 卡根目录），不需要自己转换。注意：
  - 一个选项码数超过一行上限（36 条）时会拆成多行，需要逐行开启。
  - 同一个游戏代码有多套（对应不同汉化版本）时，第一套保存为 `XXXY.cht`，其余保存为 `XXXY~2.cht`
    等不会被自动载入的名字，需要时自己改名。
  - 这些金手指的数据来自 open_agb_firm 项目自带的金手指合集（原作者及整理者见该项目），本分支只做了格式转换，
    权利归原作者所有。如有异议请联系我删除。
- 官方金手指库：[davidgfnet/superfw-cheats](https://github.com/davidgfnet/superfw-cheats)（约 140 个游戏，不含汉化版）。
  它没有声明授权，所以没有放进本分支的发布包，需要的话请到它的 Releases 页面自行下载，和汉化金手指放在同一个目录即可，文件名不会冲突。
- 使用金手指需要在设置里打开"游戏内菜单"和"金手指"。

## 安装

> 先阅读上游文档：<https://superfw.davidgf.net/docs/install/flash/>

1. 从 [Releases](../../releases) 下载。最省事的是 `superfw-chis-sd-files-v0.22.1.zip`：里面是 `superfw-chis.fw` 和
   `.superfw/cheats/` 下的汉化金手指，直接解压到 SD 卡根目录。只想要固件的话单独下载 `superfw-chis.fw`；
   已经装了金手指、只想补充汉化金手指的话下载 `superfw-cheats-zh-v0.22.1.zip`。
2. **先用链式加载试用（推荐）**：把文件复制一份并改名为 `.gba`（例如 `superfw-chis-test.gba`），
   在已装的 SuperFW（或默认固件 / SCFW）的文件浏览器里像普通游戏一样运行。不会改动任何东西，重启就回到原来的固件。
3. 确认好用后再刷写：在"信息"窗口里同时按住 **下 + B + START** 解锁刷写（当次开机有效），
   回到浏览页选中 `.fw` 文件，按 A，再按 **L + R + 上** 开始。
   刷写过程中不要断电，刷完后建议重启 GBA。
4. 如果刷坏了，可以用 NDS + Slot-1 烧录卡恢复，见上游文档
   [superfw-nds-flasher-tool](https://github.com/davidgfnet/superfw-nds-flasher-tool/releases/)。

## 编译

用 GitHub Actions 编译（`.github/workflows/build-release.yml`，只编译 `chis`）。
推送到 `master` 会生成构建产物，推送标签会自动创建 Release 并附上固件。
本地编译需要 `arm-none-eabi-gcc`，命令与上游相同：

```
make BOARD=chis COMPRESSION_RATIO=10
```

## 已知问题与说明

- 这些改动主要是在电脑上按像素尺寸计算和模拟渲染后写成的，真机上的观感（字大小、窗口位置、截断）可能还需要微调。
- 只在 SuperChis 上使用过，其他型号请自行编译测试。
- 游戏加载窗口（信息 / 加载设置 / 补丁）仍然用 L / R 切换子页，没有改动。
- 非中文语言里新增的几条文字（标签、"START/Menu"）暂时是英文。

## 许可与致谢

- SuperFW 由 David Guillen Fandos 开发，GPL-3.0，本分支沿用同一许可。上游项目：<https://github.com/davidgfnet/superfw>
- 字体：Fusion Pixel Font（TakWolf），SIL Open Font License 1.1，见 `res/fonts/LICENSE-fusion-pixel-OFL.txt`。
  其中包含 Ark Pixel、Cubic 11、Galmuri 等字体的成分，授权见该字体发布包。
- 原有字体：Unscii（viznut）和 GNU Unifont，见 `res/fonts/README.md`。
- 金手指转换工具针对 open_agb_firm 的 `gba.acl` 格式编写，感谢该项目和金手指合集的整理者。
