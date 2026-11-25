# SuperFW 中文增强分支（0.22.x）

这是 [davidgfnet/superfw](https://github.com/davidgfnet/superfw)（Supercard 的开源固件）的一个分支，
基于上游 **v0.22**，版本号显示为 **0.22.1**（上游版本号不变，后面加的是本分支自己的小版本号）。

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
  - 原来的"设置"和"界面/语言"合并成一个设置窗口（主题、语言、动画速度、显示隐藏文件在列表末尾）。
  - 设置窗口里 L / R 不再切换页面，只用方向键左右修改数值；底部是当前项的说明（过长会滚动）。
  - "保存"按钮同时写入 `settings.txt` 和 `ui-settings.txt`。
  - 信息页的固件更新解锁组合键（下 + B + START）保持不变，窗口里同样有效。
- 信息页显示 `Version 0.22.1`。小版本号在 `Makefile` 的 `CUSTOM_VERSION`，只用于显示，不影响固件更新的版本判断。

### 字体

- 汉字和英文改用 [Fusion Pixel Font](https://github.com/TakWolf/fusion-pixel-font) 12px 点阵字体（SIL OFL 1.1）。
  汉字 12×12，比原来的 16×16 窄 25%；英文改成比例字体，平均窄约 17%。
- 字体包从 697 KB 减到 528 KB。
- 汉字覆盖 CJK 统一表意文字区的约 94%（简体为主，繁体和日文版补充），其余生僻字显示空心方框。
- 韩文、俄文、希腊文、带音标的拉丁字母仍使用原来的 16 像素字体。
- 字体生成工具见 `res/fonts/`：`fusion2hex.py`（BDF 转换）和 `generator.py`（新增 12 像素宽块类型），
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
