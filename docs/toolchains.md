# 工具链

本机已为 th18 重建项目准备并验证的工具。路径均相对项目根 `C:\Users\87804\Documents\playground\th18`。

## 专用工具（本次安装）

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

## 尚未安装（需要时再装）

- Emscripten SDK 6.0.9 — 阶段② Web Runtime 编译
- WASI SDK 34 — 可选的无头核心验证
