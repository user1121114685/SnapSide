# IconMover

IconMover 是一个原生 C++/Win32 桌面托盘工具，用于在多显示器场景下，把 Windows 桌面 `SysListView32` 中的图标坐标移动到目标显示器工作区。它只修改桌面 ListView 坐标，不移动桌面文件，也不操作文件系统中的快捷方式或实际文件。

## 功能摘要

- 启动后等待 Explorer 桌面 `SysListView32` 就绪，并自动执行一次图标移动。
- 托盘菜单支持“立即移动图标”“设置目标显示器”“开机自启”“退出”。
- 使用 `WM_DISPLAYCHANGE` 监听显示器变化，自动重新枚举拓扑并重排图标。
- 开机自启状态保存在 `HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run`。
- 日志固定写入 `%APPDATA%\\IconMover\\icon_mover.log`。

## 环境要求

- Windows 10 x64
- CMake 3.21 或更高
- MSVC 工具链，Visual Studio 2019/2022 或等效 Build Tools
- Explorer 作为默认桌面 Shell

## 构建

建议在已加载 MSVC x64 环境的 Developer PowerShell 中执行，或复用仓库里的 `agent.py` 自动探测并加载可用环境。

当前仓库在本机默认按下面的顺序构建：

```powershell
cmd /c "\"D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat\" && cmake -S . -B build && cmake --build build --config Release"
```

当前仓库验证优先使用 NMake 生成器：

```powershell
cmake -S . -B build -G "NMake Makefiles"
cmake --build build --config Release
```

如果本机已经探测到对应的 Visual Studio CMake generator，也可以改用 Visual Studio 生成器：

```powershell
cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64
cmake --build build-vs --config Release
```

## 构建注意事项

- 编译前先加载 `D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat`，否则 `cl.exe`、`rc.exe`、`nmake` 和对应 SDK 环境可能不完整。
- 不要在同一个 `build` 目录里来回切换 `NMake Makefiles` 和 `Visual Studio 17 2022` 生成器；需要切换时请改用新的构建目录，或先清理旧缓存。
- 如果 `IconMover.exe` 正在运行，链接阶段可能报 `LNK1168`，因为输出文件被占用。重新编译前先退出正在运行的程序，或改用新的构建目录。
- 仓库中的 `.cpp`、`.h`、`.rc` 和顶层 `CMakeLists.txt` 需保持为 UTF-8 with BOM，避免 MSVC 与资源编译器对中文和资源路径解析不一致。

## 运行

NMake 构建默认输出：

```powershell
.\build\IconMover.exe
```

Visual Studio 生成器默认输出：

```powershell
.\build-vs\Release\IconMover.exe
```

辅助启动参数：

- `--set-target-monitor=<monitor_id>`：直接更新配置中的目标显示器并退出。
- `--log-test-error`：写入一条受控错误日志，便于检查 `GetLastError() + FormatMessage` 日志路径。

## 运行时数据

- 日志：`%APPDATA%\\IconMover\\icon_mover.log`
- 配置：`%APPDATA%\\IconMover\\config.ini`
- 开机自启注册表：`HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\IconMover`

## 发布前人工验收

以下步骤覆盖 PRD 的重点场景。每项都应同时观察桌面实际结果和日志文件。
如果这份记录用于发布签收，步骤必须通过真实托盘 UI 操作和真实显示器拓扑变化完成；向隐藏窗口发送 `WM_COMMAND` / `WM_DISPLAYCHANGE`，或写入 `INVALID_MONITOR`，只能算功能等效覆盖，不算人工验收通过。

### 1. 双屏启动自动移动

步骤：

1. 连接主屏和至少一个副屏，确保 Explorer 为当前桌面 Shell。
2. 清空旧日志或记录当前日志末尾时间戳。
3. 启动 `IconMover.exe`，等待托盘图标出现并等待 Explorer 桌面就绪。

预期：

- 图标在启动后自动移动到默认副屏，或移动到配置中保存的目标显示器。
- 日志中能看到启动、Explorer 就绪、目标显示器解析、图标移动开始/完成的完整记录。

### 2. 手动“立即移动图标”

步骤：

1. 右键托盘图标。
2. 点击“立即移动图标”。

预期：

- 图标再次执行一次完整重排。
- 日志中能看到托盘命令分发、应用层移动调度、桌面移动执行结果。

### 3. 切换目标显示器

步骤：

1. 在多副屏环境下，右键托盘图标并展开“设置目标显示器”。
2. 选择另一个非主显示器。

预期：

- 配置立即持久化，图标立刻重排到新目标显示器工作区。
- 日志中能看到目标显示器选择、配置保存、目标显示器解析结果和移动结果。

### 4. 开机自启开关

步骤：

1. 右键托盘图标并切换“开机自启”。
2. 使用 `reg query "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run" /v IconMover` 检查结果。
3. 再次切换回原状态并重新检查。

预期：

- 菜单勾选状态与注册表真实值一致。
- 日志中能看到自启状态读取、写入、校验和最终有效状态。

### 5. 断开副屏降级

步骤：

1. 在程序常驻时断开当前目标副屏，或关闭扩展坞输出。
2. 等待 `WM_DISPLAYCHANGE` 防抖和自动重排完成。

预期：

- 程序不崩溃。
- 如果仍有其他副屏，则自动降级到首个可用副屏并重排。
- 如果只剩主屏，则本次移动跳过，但日志完整记录降级或跳过原因。

### 6. 重连副屏自动重排

步骤：

1. 重新连接之前断开的副屏。
2. 等待 Explorer 和显示器拓扑稳定。

预期：

- 程序会自动重新枚举显示器并再次触发图标重排。
- 日志中能看到 `WM_DISPLAYCHANGE`、防抖、Explorer 就绪等待、重排执行和结果。

### 7. 退出与日志收尾

步骤：

1. 右键托盘图标并点击“退出”。

预期：

- 托盘图标被移除，进程退出。
- 日志中能看到 shutdown 原因、定时器状态、托盘移除和消息循环退出。

## 2026-04-03 回归结果

2026-04-03 已在本机完成一轮发布前回归记录，完整记录见 [reviews/task_017_validation_summary.md](reviews/task_017_validation_summary.md)。当前结论为：构建、版本资源、关键日志和 UTF-8 with BOM 检查通过，但 PRD 要求的发布前人工验收尚未全部完成。

- 构建执行对象：`build\\IconMover.exe`
- 回归环境：Windows 11 x64，`\\.\DISPLAY1` 主屏 + `\\.\DISPLAY2` 副屏
- 已完成并可复核的结果：`cmake -S . -B build`、`cmake --build build --config Release` 通过，版本资源已嵌入，`src/`、`include/`、`*.rc` 与顶层 `CMakeLists.txt` 共 38 个文件 UTF-8 with BOM 检查通过
- 已完成的功能运行验证：双屏启动自动移动、日志关键阶段完整记录、注册表写入/删除后的外部回读、退出路径清理
- 部分覆盖，不能写成 PRD 人工验收通过：
  - “立即移动图标”“开机自启”“退出”验证的是应用真实命令分发路径，但本次记录不是托盘 UI 实际点击
  - “切换目标显示器”只验证了菜单生成、配置持久化和对唯一副屏 `\\.\DISPLAY2` 的即时重排；由于当次拓扑只有 1 个可选副屏，未覆盖“切换到另一个副屏”
  - “断开副屏降级”“重连副屏自动重排”分别只做了 `INVALID_MONITOR` 降级分支和 `WM_DISPLAYCHANGE` 处理链路的等效覆盖，未完成真实副屏断开/重连热插拔回归
- 发布前仍需补测：
  - 至少两个可选非主屏的环境下，使用真实托盘 UI 验证“切换目标显示器”
  - 真实断开当前目标副屏，验证降级或跳过行为
  - 真实重连副屏，验证自动重排
  - 使用真实托盘 UI 补齐“立即移动图标”“开机自启”“退出”的人工操作记录

## 编码要求

仓库中的 `.cpp`、`.h`、`.rc` 和顶层 `CMakeLists.txt` 均应保持为 UTF-8 with BOM，便于 MSVC 和资源编译器一致处理。
