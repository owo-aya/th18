# 工具链

TH18 重建项目的本地工具链与社区参考索引。路径均相对项目根 `C:\Users\87804\Documents\playground\th18`；基础工具、本地参考源码和已解压发行工具分别记录使用状态。

## 已配置的基础工具

### Ghidra 12.2 PUBLIC — `toolchains/ghidra_12.2_PUBLIC/`

二进制逆向分析，阶段①重建 th18.exe 的主力。

- **本机 JDK 25 = Minecraft 自带运行时，是完整 JDK（含 `javac`/`javadoc`/`jmod`），不是 JRE。** 路径：

```
C:\Users\87804\AppData\Roaming\.minecraft\runtime\java-runtime-epsilon
```

  实测 `javac 25.0.1`、`openjdk version "25.0.1" 2025-10-21 LTS`。Ghidra 12.2 headless 与脚本编译（Java postScript、jython/pyghidra 均可）全部用这个：

```powershell
$env:JAVA_HOME = 'C:\Users\87804\AppData\Roaming\.minecraft\runtime\java-runtime-epsilon'
& 'toolchains\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat' <project_location> <project_name> -import <exe>
```

- **Python 查询接口**：`pip install pyghidra`（3.2.0）后可用 Python 直接开工程查询/反编译，配合 JDK 25 无需另装 Java：

```powershell
$env:GHIDRA_INSTALL_DIR = 'C:\Users\87804\Documents\playground\th18\toolchains\ghidra_12.2_PUBLIC'
$env:JAVA_HOME = 'C:\Users\87804\AppData\Roaming\.minecraft\runtime\java-runtime-epsilon'
python toolchains\re\analyze_basics.py
```

  API：`pyghidra.open_project(project_location, project_name)` + `with pyghidra.program_context(proj, "/th18.exe") as prog:`；`open_program()` 在 3.2 已弃用。
- 坑：`analyzeHeadless.bat` 内部走 cmd，路径里带 `[` `]` 会被 cmd 解析坏（报 `\th18.exe was unexpected at this time`）——先把 exe 复制到无括号路径（现有副本 `toolchains\ghidra-projects\source\th18.exe`）再导入；`project_location` 目录必须先存在。

### thtk @ 917a3d3 — `toolchains/thtk/build/`

东方资源档工具（静态编译，MinGW GCC 16.1 + winflexbison）。**原生支持 th18，无需打补丁**（th20 才需要）。子模块 libpng/zlib-ng/thtypes 已拉取。

| 工具 | 位置 | 用途 |
|---|---|---|
| thdat | `toolchains/thtk/build/thdat/thdat.exe` | DAT 档案列目录/抽取/回编 |
| thecl | `toolchains/thtk/build/thecl/thecl.exe` | ECL 弹幕脚本 ↔ 文本 |
| thstd | `toolchains/thtk/build/thstd/thstd.exe` | STD 关卡背景 |
| thmsg | `toolchains/thtk/build/thmsg/thmsg.exe` | MSG 对话 |
| thanm | `toolchains/thtk/build/thanm/thanm.exe` | ANM 图像档案（th18 为 ANM v8） |

已验证的真实调用（游戏目录 `[th18] 东方虹龙洞 (汉化版+日文版)`）：

```powershell
# 列出 th18.dat 全部内容（ANM/ECL/STD/MSG/SHT/TXT）
thdat.exe -l 18 th18.dat
# 抽取指定文件到目录
thdat.exe -C <out_dir> -x 18 th18.dat st01.ecl st01.std st01a.msg text.anm
# 单文件解析（dump 到 stdout）
thecl.exe -d 18 st01.ecl
thstd.exe -d 18 st01.std
thmsg.exe -d 18 st01a.msg
thanm.exe -l 18 text.anm     # 列 ANM 条目；-x 抽取
```

注意：thtk 各工具的选项要带 `-`（`-l 18`，不是 `l 18`）；dump/extract 输入必须是**已从 DAT 抽出的**散文件。

### ffmpeg 9.0.2（winget）

音频转码（thbgm.dat → OGG，阶段②用）。绝对路径：

```
C:\Users\87804\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0.2-full_build\bin\ffmpeg.exe
```

（winget 已加 `ffmpeg` 命令别名，新终端可直接 `ffmpeg`。）

## 社区工具与参考索引

遇到函数、字段、脚本或资源格式疑点时，先按下表查阅已有资料，再回到项目指定原版 EXE 与资源确认对应关系，只继续调查尚未解释的部分。核对后的名称与类型可用于 Ghidra，已确认的机制落实到源码。社区命名、偏移和补丁后的行为均不能替代原版依据。

### 本地版本与状态（2026-10-27）

以下源码均为本次取得的浅克隆，集中放在 `toolchains/community/`。该目录由现有 `.gitignore` 忽略，项目 Git 只管理本索引。版本列为 Git 提交标识；truth 与 apitrace 的源码版本对应所下载的发行包。

| 来源 | 本地目录 | 版本 | 本地状态 |
|---|---|---|---|
| [th-re-data](https://github.com/exphp-share/th-re-data) | [th-re-data/](../toolchains/community/th-re-data/) | `41cd633` | TH18 v1.00a JSON 已下载；尚未导入 Ghidra |
| [ParaKit](https://github.com/Guy-L/parakit) | [parakit/](../toolchains/community/parakit/) | `2f2db40` | 源码已下载；本轮未配置其 Python 依赖环境或运行采集 |
| [Priw8/eclmap](https://github.com/Priw8/eclmap) | [eclmap/](../toolchains/community/eclmap/) | `f146162` | 映射已下载；TH18 文件由上游作为占位映射加入，仍含未知指令 |
| [truth](https://github.com/ExpHP/truth) | [truth/](../toolchains/community/truth/) | `0.5.2` / `0f32abf` | 源码及 Windows x64 发行工具已下载，见下表 |
| [sht-webedit](https://github.com/Priw8/sht-webedit) | [sht-webedit/](../toolchains/community/sht-webedit/) | `98b8cca` | 页面和本地脚本已下载；未操作浏览器编辑 SHT |
| [thprac](https://github.com/touhouworldcup/thprac) | [thprac/](../toolchains/community/thprac/) | `33c78d9` | 仅作源码参考；未构建或注入游戏 |
| [threp](https://github.com/wasupandceacar/threp) | [threp/](../toolchains/community/threp/) | `c92d4cc` | 已用本机 Python 从本地源码导入 `THReplay`；未解析录像 |
| [apitrace](https://github.com/apitrace/apitrace) | [apitrace/](../toolchains/community/apitrace/) | `14.0` / `18cfd38` | 源码及 Windows x86 发行工具已下载，见下表 |
| [thcrap](https://github.com/thpatch/thcrap) | [thcrap/](../toolchains/community/thcrap/) | `0ff57b3` | 仅作源码参考；未构建或加载补丁 |

源码克隆未递归下载各项目构建子模块。当前用途是查询源码；需要构建某项工具时，再按该版本的构建说明准备依赖。

| 发行工具 | 本地入口 | 已完成的调用 |
|---|---|---|
| [truth 0.5.2 Windows x64](https://github.com/ExpHP/truth/releases/tag/0.5.2) | [truth-0.5.2/](../toolchains/community/truth-0.5.2/) 下的 `truanm.exe`、`trustd.exe`、`trumsg.exe`；同目录含 `truth-core.exe` 与 `map/` | 三个反编译入口的帮助可调用，版本返回 `truth 0.5.2` |
| [apitrace 14.0 Windows x86](https://github.com/apitrace/apitrace/releases/tag/14.0) | [apitrace.exe](../toolchains/community/apitrace-14.0-win32/bin/apitrace.exe)；D3D9 包装库位于 `apitrace-14.0-win32/lib/wrappers/d3d9.dll` | 帮助与版本可调用，版本返回 `apitrace 14.0`；未运行游戏采集 |

### 按调查问题查找

| 调查问题 | 本地入口 | 重点查找内容与使用范围 |
|---|---|---|
| 函数用途、全局对象、结构布局、分支标签 | [th-re-data/data/th18.v1.00a/](../toolchains/community/th-re-data/data/th18.v1.00a/) | `funcs.json`、`statics.json`、`type-structs-own.json`、`labels.json`。按函数地址或 `AbilityManager`、`AnmVm`、`Timer`、`Rng` 等名字搜索；结构中的未知区域继续以原版为依据调查 |
| 子弹、敌人、激光、道具、自机弹、卡牌状态 | [offsets.py](../toolchains/community/parakit/offsets.py)、[game_entities.py](../toolchains/community/parakit/game_entities.py)、[state_reader.py](../toolchains/community/parakit/state_reader.py) | `offsets.py` 的 `'th18.exe'` 段、`GameStateUM`、`ActiveCard`、`extract_bullets`、`extract_enemies`、`extract_lasers` 及 `game_id == 18` 分支。复用所需字段与遍历线索；`exact` 轮询暂停不等于双方停在同一逻辑边界，项目继续使用现有帧同步控制器 |
| ECL 编号难读、Boss 或弹幕脚本定位 | [th18.eclm](../toolchains/community/eclmap/eclmap/th18.eclm) | 配合现有 `thecl -m` 显示助记符；映射含占位和未知项，名称不能证明指令语义完整 |
| ANM、STD、MSG 的控制流难读 | [truth README](../toolchains/community/truth/README.md)、[语法说明](../toolchains/community/truth/doc/syntax.md)、[发行映射目录](../toolchains/community/truth-0.5.2/map/) | `truanm`、`trustd`、`trumsg` 及 `any.anmm`、`any.stdm`、`any.msgm`；辅助阅读循环、条件与跳转。TH18 ECL 继续使用 thecl |
| 四机体射击参数、火力分组、回调编号 | [sht-webedit/index.html](../toolchains/community/sht-webedit/index.html)、[struct_18.js](../toolchains/community/sht-webedit/js/struct/struct_18.js)、[README](../toolchains/community/sht-webedit/README.md) | 本地页面可作为打开入口；字段定义包含射速、初始延迟、伤害、偏移、子机和回调编号。追踪、命中、爆炸等硬编码回调仍需分析原版 EXE |
| 卡牌、商店、录像流程与失步原因 | [thprac_th18.cpp](../toolchains/community/thprac/thprac/src/thprac_th18.cpp) | `enum addrs`、`CardBase`、`AbilityManager`、`ReplayStageData`、`Replay`，以及 `shop`、`desync` 相关代码。区分原版字段/入口与 thprac 改写逻辑，不能用补丁后的行为替代原版 |
| 录像解码、分关数据、按键序列与帧号 | [static.py](../toolchains/community/threp/threp/static.py)、[decode.py](../toolchains/community/threp/threp/decode.py)、[api.py](../toolchains/community/threp/threp/api.py) | `static.py` 的 `'18'` 项、`threp_decodedata`、`threp_cut`、`THReplay`、`getKeyboardAction`、`getZ`、`getX`、`getC`。与原版读写逻辑核对；商店和跨关状态仍走真实流程 |
| 绘制顺序、混合、深度、雾与纹理状态 | [apitrace 使用说明](../toolchains/community/apitrace/docs/USAGE.markdown)、[D3D9 调用定义](../toolchains/community/apitrace/specs/d3d9.py) | 按具体渲染疑点使用 32 位发行工具调查 D3D9 调用。会生成临时 trace，实际运行与采集遵循 AGENTS.md 的授权范围 |
| BGM 格式与循环点含义 | [thcrap BGM](../toolchains/community/thcrap/thcrap_tsa/src/bgm.cpp)、[bgm_types.h](../toolchains/thtk/extlib/thtypes/bgm_types.h) | `bgm_fmt_t`、`bgm_fmt`、`thbgm`、`loop` 相关实现；与原始音乐描述数据及播放调用核对。已有 `thtypes` 还提供 `anm_types.h`、`ecl_types.h`，无需重复下载 |

### 查询与调用入口

从项目根目录执行。以下资源命令为调用模板，替换输入路径；本轮已核对工具帮助，未执行资源反编译或游戏采集。

```powershell
# 原版对象、字段与函数命名线索
rg -n 'AbilityManager|Timer|Rng' toolchains/community/th-re-data/data/th18.v1.00a
rg -n 'zCard_|zAbilityManager_' toolchains/community/parakit/offsets.py
rg -n 'CardBase|AbilityManager|ReplayStageData|desync' toolchains/community/thprac/thprac/src/thprac_th18.cpp

# thtk + TH18 助记符
& toolchains/thtk/build/thecl/thecl.exe -d 18 -m toolchains/community/eclmap/eclmap/th18.eclm '<已提取的ECL路径>'

# truth：默认将反编译文本输出到终端
& toolchains/community/truth-0.5.2/truanm.exe decompile -g18 -m toolchains/community/truth-0.5.2/map/any.anmm '<已提取的ANM路径>'
& toolchains/community/truth-0.5.2/trustd.exe decompile -g18 -m toolchains/community/truth-0.5.2/map/any.stdm '<已提取的STD路径>'
& toolchains/community/truth-0.5.2/trumsg.exe decompile -g18 -m toolchains/community/truth-0.5.2/map/any.msgm '<已提取的MSG路径>'

# apitrace：查看 D3D9 采集参数，不启动游戏
& toolchains/community/apitrace-14.0-win32/bin/apitrace.exe help trace
```

threp 可直接从本地源码导入，无需安装到全局 Python；传入实际录像后再调用读取接口：

```python
import sys
sys.path.insert(0, "toolchains/community/threp")
from threp import THReplay
```

## 系统已有工具（无需安装）

| 工具 | 版本 | 备注 |
|---|---|---|
| VS 2022 Build Tools | MSVC 14.44，x64 + **x86** cl 都有 | 阶段①本机源码构建（Win32/x86 目标） |
| Windows SDK | 10.0.26100.0 | |
| CMake + Ninja | 4.4.1 / 1.13.2（WinLibs 版） | |
| MinGW-w64 GCC | 16.1 UCRT | 编 thtk 用的就是它；`libgomp-1.dll` 等运行库在其 bin 目录（已在系统 PATH） |
| Node.js / npm | 24.18.1 / 11.16 | 上游 Launcher 构建要求 ≥22.18 |
| Python | 3.12.10 | |
| Git | 2.56 | |
| ripgrep | 14.1.1 | 上游打包脚本需要 |
| winflexbison | 2.5.25 | 编 thtk 时作为 `-DBISON_EXECUTABLE`/`-DFLEX_EXECUTABLE` 传入 |

## 环境注意（踩过的坑）

1. **DSH 的 bash 执行器全局只读**：`ops_bash` 里 touch/mkdir 在任何位置都被拒绝。一切命令改用 **pwsh** 执行。
2. **`[th18]` 目录名**：PowerShell 的 `-Path` 把 `[...]` 当通配符。进游戏目录、引用其中文件一律用 **`-LiteralPath`**。
3. thtk 可执行文件静态链了 libthtk/zlib/libpng，但仍动态依赖 `libgomp-1.dll`（位于 WinLibs 的 mingw64\bin，在系统 PATH 里，正常可用）。
4. 2026-10-27 下载社区工具时，命令行直连 GitHub 无响应，Windows 已启用的代理为 `127.0.0.1:7890`。本次通过单次命令参数 `git -c http.proxy=http://127.0.0.1:7890 ...` 和 `curl.exe --proxy http://127.0.0.1:7890 ...` 完成下载；后续连接问题先查看当时实际代理设置。

## 尚未安装（需要时再装）

- Emscripten SDK 6.0.9 — 阶段② Web Runtime 编译
- WASI SDK 34 — 可选的无头核心验证
