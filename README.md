# IconMover

IconMover 是一个原生 C++/Win32 桌面托盘工具，用于在多显示器场景下，把 Windows 桌面 `SysListView32` 中的图标优先排列到目标显示器工作区。它只修改桌面 ListView 坐标，不移动桌面文件，也不操作文件系统中的快捷方式或实际文件。

## 功能摘要

- 启动后等待 Explorer 桌面 `SysListView32` 就绪，并自动执行一次图标移动。
- 登录自启时先请求 Windows 使用“扩展这些显示器”拓扑，再等待显示器和 Explorer 稳定。
- 目标显示器空间不足时，使用各显示器的原生 DPI 网格，依次溢出到其他副屏和主屏，避免重叠。
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

在已加载 MSVC x64 环境的 Developer PowerShell 中直接使用 CMake 构建。

在 Developer PowerShell 中，推荐使用 NMake 生成器：

```powershell
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

如果本机已经探测到对应的 Visual Studio CMake generator，也可以改用 Visual Studio 生成器：

```powershell
cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64
cmake --build build-vs --config Release
```

## 构建注意事项

- 编译前先加载 `D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat`，否则 `cl.exe`、`rc.exe`、`nmake` 和对应 SDK 环境可能不完整。
- 不要在同一个 `build` 目录里来回切换 `NMake Makefiles` 和 `Visual Studio 17 2022` 生成器；需要切换时请改用新的构建目录，或先清理旧缓存。
- `NMake Makefiles` 是单配置生成器，`cmake --build build --config Release` 不会切换到 Release；要生成 Release，请在配置阶段显式传入 `-DCMAKE_BUILD_TYPE=Release`。
- 如果配置阶段出现 `CMakeFiles\\cmTC_*.dir\\build.make` 找不到，通常是当前 `build\\CMakeFiles\\CMakeScratch` 的临时状态损坏；先删除或更换构建目录，再重新执行配置即可。
- 如果 `IconMover.exe` 正在运行，链接阶段可能报 `LNK1104`/`LNK1168`，因为输出文件被占用。重新编译前先退出正在运行的程序，或改用新的构建目录。
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
- `--autostart`：登录自启专用；先请求扩展显示器，再延迟执行图标重排。手动启动不强制切换显示模式。
- `--log-test-error`：写入一条受控错误日志，便于检查 `GetLastError() + FormatMessage` 日志路径。

## 运行时数据

- 日志：`%APPDATA%\\IconMover\\icon_mover.log`
- 配置：`%APPDATA%\\IconMover\\config.ini`
- 开机自启注册表：`HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\IconMover`，命令行为 `"<IconMover.exe>" --autostart`。旧版不带参数的注册项会自动迁移。

## 多 DPI 与显示变化

桌面 `SysListView32` 属于 Explorer 进程，`LVM_GETITEMSPACING` 返回的间距可能按主屏 DPI 虚拟化，而图标坐标最终要写入目标副屏的坐标系。移动器会读取目标显示器的有效 DPI，将 ListView 间距转换到目标 DPI 后再计算网格。例如主屏 200%、副屏 100% 时，`152x204` 会归一化为 `76x102`。

程序不会用小于显示器原生 DPI 网格的间距强塞图标，因为 Explorer 会把这些坐标重新吸附并造成重叠。目标副屏容量不足时，先填满目标副屏，再按“其他副屏、主屏”的顺序排列剩余图标。

普通启动自动移动会等待 1.5 秒；登录自启会先调用 `SetDisplayConfig(SDC_TOPOLOGY_EXTEND)`，再等待 4 秒。`WM_DISPLAYCHANGE` 会合并 2 秒内的连续变化，给 Windows 和 Explorer 留出完成分辨率、缩放和桌面视图重建的时间。应用后会逐个读取图标坐标，并检查显示器归属和重复坐标；永久布局错误不会进入无限重试。

## 发布前人工验收

以下步骤覆盖 PRD 的重点场景。每项都应同时观察桌面实际结果和日志文件。
如果这份记录用于发布签收，步骤必须通过真实托盘 UI 操作和真实显示器拓扑变化完成；向隐藏窗口发送 `WM_COMMAND` / `WM_DISPLAYCHANGE`，或写入 `INVALID_MONITOR`，只能算功能等效覆盖，不算人工验收通过。

### 1. 双屏启动自动移动

步骤：

1. 连接主屏和至少一个副屏，确保 Explorer 为当前桌面 Shell。
2. 清空旧日志或记录当前日志末尾时间戳。
3. 使用登录自启，或以 `IconMover.exe --autostart` 启动，等待托盘图标出现并等待 Explorer 桌面就绪。

预期：

- Windows 切换到扩展显示器模式。
- 图标优先移动到默认副屏或配置中的目标显示器；超出目标网格容量的图标整齐排列到其他活动显示器。
- 日志中能看到扩展模式请求、Explorer 就绪、目标显示器解析、各屏容量和图标移动结果。

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

- 配置立即持久化，图标优先重排到新目标显示器工作区，溢出部分使用其他活动显示器。
- 日志中能看到目标显示器选择、配置保存、目标显示器解析结果和移动结果。

### 4. 开机自启开关

步骤：

1. 右键托盘图标并切换“开机自启”。
2. 使用 `reg query "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run" /v IconMover` 检查结果。
3. 再次切换回原状态并重新检查。

预期：

- 菜单勾选状态与注册表真实值一致。
- 注册表命令行包含 `--autostart`。
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

## 编码要求

仓库中的 `.cpp`、`.h`、`.rc` 和顶层 `CMakeLists.txt` 均应保持为 UTF-8 with BOM，便于 MSVC 和资源编译器一致处理。
