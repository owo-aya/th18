# TH18 · 东方虹龙洞源码重建

《东方虹龙洞 ～ Unconnected Marketeers.》日文 Windows v1.00a 的 C++ 源码重建工程，目标是按原版机制实现 1:1 还原。

**当前阶段：Windows 原生版还原中。** 工程已有游戏流程、资源读取、渲染、音频、自机、敌机、弹幕、卡牌、商店、录像与存档等实现，仍在逐项核对原版并修复差异。编译通过不代表整局行为已达到 1:1；游戏实测由维护者进行。Web Runtime 与统一 Launcher 接入尚未完成。

## 构建

当前目标为 **Windows / x86 / C++20**，使用 Win32、Direct3D 9 与 DirectSound。

需要：

- Visual Studio 2022 Build Tools，安装 C++ x86 工具链与 Windows SDK。
- CMake 3.24 或更新版本。
- Ninja，并将 CMake、Ninja 加入 PATH。

```powershell
git clone https://github.com/owo-aya/th18.git
Set-Location th18
.\ports\th18\portable\build.cmd
```

脚本使用 VS 2022 Build Tools 默认安装位置 `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`。其他 VS 安装位置可在对应的 **x86 Native Tools Command Prompt** 中直接构建：

```bat
cmake -S ports/th18 -B ports/th18/build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ports/th18/build/native
```

产物为 `ports/th18/build/native/th18.exe`。源码构建不需要原版游戏文件。

## 运行资源

运行时需要自行提供日文原版 v1.00a 的 `th18.dat` 与 `thbgm.dat`，放在同一个本地目录中。原版 EXE 不参与重建版运行。

现有启动入口：

```powershell
.\ports\th18\portable\run.cmd
```

该脚本从仓库下被 Git 忽略的 `toolchains/game18/` 读取资源，将配置、存档与录像写入 `ports/th18/build/native/`。

也可以直接指定自己的资源目录。下面的路径需替换为实际的绝对路径，用户目录需事先存在：

```powershell
& .\ports\th18\build\native\th18.exe --game-dir 'C:\Games\TH18' --user-dir 'C:\Games\TH18-Reconstruction-User'
```

默认键位：方向键移动，Z 确认／射击，X Bomb，Shift 低速，C 使用主动卡，D 切换主动卡，Esc 暂停。

## 目录

| 路径 | 内容 |
| --- | --- |
| `ports/th18/src/` | 游戏逻辑、Win32 平台层、资源与存档实现 |
| `ports/th18/CMakeLists.txt` | 原生构建目标 |
| `ports/th18/portable/` | Windows 构建与启动入口 |
| `toolchains/re/` | Git 跟踪的原版分析入口与 TH18 对照工具源码 |
| `docs/` | 机制笔记、修复交接与本地工具用法 |

[未解决机制交接](docs/2026-10-29-未解决机制补充与修复交接.md)记录已定位的问题与实现入口；各项状态需结合后续提交查看。[工具链说明](docs/toolchains.md)记录 Ghidra、thtk 与社区参考资料。原版游戏、工具安装、临时分析输出、构建产物及存档留在本地。

## 与 Eagler Touhou 的关系

本仓库独立维护 TH18 源码与修复历史，目录沿用 `ports/th18/`，以便后续接入 [Eagler Touhou](https://github.com/Ritosa2333/eagler-touhou)。当前上游[作品进度表](https://github.com/Ritosa2333/eagler-touhou/blob/main/docs/TOUHOU_PROGRESS.md)将 TH18 标为还原中。

## 归属与许可

原作程序、角色、图像、文字、音乐等权利属于上海爱丽丝幻乐团／ZUN 及各自权利人。仓库保留的实测截图用于说明具体实现问题，原画面权利不变。

本仓库尚未指定源码的开源许可证。第三方工具与参考项目遵循各自许可证；来源见[工具链说明](docs/toolchains.md)。详见 [NOTICE.md](NOTICE.md)。
