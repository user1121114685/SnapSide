# =====================================
# Win32 桌面图标移动工具 - AI Agent
# 专用于 C++/Win32 项目开发
# Planner (Codex) + Executor (Codex) + Critic (Codex)
# =====================================
#
# 使用方式：
#   python agent.py                       # 交互式输入需求
#   python agent.py --req "做一个托盘工具"  # 直接传需求
#   python agent.py --prd                 # 跳过PRD生成，直接读 prd.txt
#   python agent.py --tasks               # 跳过PRD+拆任务，直接读 tasks.json
#   python agent.py --reset-retries       # 重置失败任务，重新执行
#set AGENT_VCVARS_PATH="D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"


from __future__ import annotations

import json
import subprocess
import argparse
import sys
import io
import difflib
import queue
import shutil
import threading
import time
import re
import tempfile

from pathlib import Path
from datetime import datetime
import os


# =========================
# UTF-8 控制台配置
# =========================
def configure_utf8_console():
    os.environ["PYTHONUTF8"] = "1"
    os.environ["PYTHONIOENCODING"] = "utf-8"
    if os.name == "nt":
        try:
            import ctypes
            kernel32 = ctypes.windll.kernel32
            kernel32.SetConsoleCP(65001)
            kernel32.SetConsoleOutputCP(65001)
        except Exception:
            pass
    for name in ("stdin", "stdout", "stderr"):
        stream = getattr(sys, name, None)
        if stream is None:
            continue
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
            continue
        except (AttributeError, ValueError):
            pass
        buffer = getattr(stream, "buffer", None)
        if buffer is None:
            continue
        wrapped = io.TextIOWrapper(
            buffer,
            encoding="utf-8",
            errors="replace",
            line_buffering=(name != "stdin"),
        )
        setattr(sys, name, wrapped)


_VCVARS_ENV_CACHE: dict | None = None
_CMAKE_CAPABILITIES_CACHE: dict | None = None
_CMAKE_GENERATOR_PROBE_CACHE: dict[tuple[str, str], tuple[bool, str]] = {}
_EXECUTION_ENV_GUIDANCE_CACHE: str | None = None

_VCVARS_CANDIDATES = [
    # D 盘 VS 2022（用户实际路径优先）
    r"D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
    r"D:\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
    r"D:\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
    r"D:\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
    # D 盘 VS 2019
    r"D:\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat",
    r"D:\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat",
    r"D:\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
    # C 盘 VS 2022
    r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
    r"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
    r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
    r"C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
    # C 盘 VS 2019
    r"C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat",
    r"C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat",
    r"C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
    r"C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
]


def _iter_existing_vcvars_candidates() -> list[str]:
    if os.name != "nt":
        return []

    user_path = os.environ.get("AGENT_VCVARS_PATH", "")
    ordered = []
    seen = set()
    for bat in ([user_path] if user_path else []) + _VCVARS_CANDIDATES:
        normalized = bat.strip()
        if not normalized:
            continue
        key = normalized.lower()
        if key in seen:
            continue
        seen.add(key)
        if Path(normalized).is_file():
            ordered.append(normalized)
    return ordered


def get_preferred_vcvars_path() -> str | None:
    candidates = _iter_existing_vcvars_candidates()
    return candidates[0] if candidates else None


def has_msvc_toolchain() -> bool:
    return bool(
        get_preferred_vcvars_path()
        or shutil.which("cl")
        or shutil.which("nmake")
        or shutil.which("msbuild")
    )


def _load_vcvars_env() -> dict | None:
    """尝试加载 vcvars64.bat 并返回其导出的环境变量，失败返回 None。"""
    global _VCVARS_ENV_CACHE
    if _VCVARS_ENV_CACHE is not None:
        return _VCVARS_ENV_CACHE

    if os.name != "nt":
        _VCVARS_ENV_CACHE = {}
        return _VCVARS_ENV_CACHE

    for bat in _iter_existing_vcvars_candidates():
        try:
            # 使用 cmd /c 确保 bat 文件在独立 shell 中运行，避免引号嵌套问题
            cmd_str = f'cmd /c ""{bat}" >nul 2>&1 && set"'
            result = subprocess.run(
                cmd_str,
                shell=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=60,
            )
            if result.returncode != 0:
                continue
            env = {}
            for line in result.stdout.splitlines():
                if "=" in line:
                    k, _, v = line.partition("=")
                    env[k.strip()] = v.strip()
            if env:
                _VCVARS_ENV_CACHE = env
                # 打印确认信息，方便调试
                print(f"\033[32m[vcvars] ✅ 已加载 MSVC 环境: {bat}\033[0m", flush=True)
                # 验证关键工具确实在 PATH 里
                new_path = env.get("PATH", env.get("Path", ""))
                lowered_path = new_path.lower()
                missing = []
                if "cl.exe" not in lowered_path and "msvc" not in lowered_path:
                    missing.append("cl.exe")
                
                if missing:
                    print(f"\033[33m[vcvars] ⚠️  PATH 中未检测到 {', '.join(missing)}，请确认 VS 安装完整\033[0m", flush=True)
                return _VCVARS_ENV_CACHE
        except Exception as exc:
            print(f"\033[33m[vcvars] ⚠️  加载失败 ({bat}): {exc}\033[0m", flush=True)
            continue

    print(
        "\033[31m[vcvars] ❌ 未找到可用的 vcvars64.bat！cmake/cl.exe 将无法使用。\n"
        "    解决方法：在启动 agent.py 之前设置环境变量：\n"
        r"    set AGENT_VCVARS_PATH=D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
        "\033[0m",
        flush=True,
    )
    _VCVARS_ENV_CACHE = {}
    return _VCVARS_ENV_CACHE


def build_cli_env():
    # os.environ on Windows is case-insensitive, but os.environ.copy() returns a case-sensitive dict.
    # To prevent duplicate keys like 'PATH' and 'Path' which confuse some tools, we merge case-insensitively.
    env = os.environ.copy()
    vcvars = _load_vcvars_env()
    if vcvars:
        for k, v in vcvars.items():
            # Search for existing key in a case-insensitive way
            target_key = k
            for existing_key in list(env.keys()):
                if existing_key.lower() == k.lower():
                    target_key = existing_key
                    break
            env[target_key] = v
    
    # Ensure Python UTF-8 settings
    env["PYTHONUTF8"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    # Force CI mode to avoid interactive prompts
    env["CI"] = "true"
    env["CONTINUOUS_INTEGRATION"] = "true"
    return env


def get_cmake_capabilities() -> dict:
    global _CMAKE_CAPABILITIES_CACHE
    if _CMAKE_CAPABILITIES_CACHE is not None:
        return _CMAKE_CAPABILITIES_CACHE

    if not shutil.which("cmake"):
        _CMAKE_CAPABILITIES_CACHE = {}
        return _CMAKE_CAPABILITIES_CACHE

    ok, output = run_cmd([resolve_cli("cmake"), "-E", "capabilities"], timeout=30)
    if not ok:
        _CMAKE_CAPABILITIES_CACHE = {}
        return _CMAKE_CAPABILITIES_CACHE

    try:
        _CMAKE_CAPABILITIES_CACHE = json.loads(output)
    except json.JSONDecodeError:
        _CMAKE_CAPABILITIES_CACHE = {}
    return _CMAKE_CAPABILITIES_CACHE


def list_cmake_generators() -> set[str]:
    generators = set()
    for item in get_cmake_capabilities().get("generators", []):
        name = item.get("name")
        if name:
            generators.add(name)
    return generators


def probe_cmake_generator(generator: str, platform: str | None = None) -> tuple[bool, str]:
    key = (generator, platform or "")
    cached = _CMAKE_GENERATOR_PROBE_CACHE.get(key)
    if cached is not None:
        return cached

    if generator not in list_cmake_generators():
        result = (False, f"CMake 未列出生成器: {generator}")
        _CMAKE_GENERATOR_PROBE_CACHE[key] = result
        return result

    with tempfile.TemporaryDirectory(prefix="snapside-cmake-probe-") as probe_root:
        source_dir = Path(probe_root) / "src"
        build_dir = Path(probe_root) / "build"
        source_dir.mkdir(parents=True, exist_ok=True)
        (source_dir / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.21)\n"
            "project(AgentProbe LANGUAGES CXX)\n"
            "add_executable(agent_probe main.cpp)\n",
            encoding="utf-8",
        )
        (source_dir / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")

        command = [resolve_cli("cmake"), "-S", str(source_dir), "-B", str(build_dir), "-G", generator]
        if platform:
            command.extend(["-A", platform])
        ok, output = run_cmd(command, cwd=str(source_dir), timeout=90)

    result = (ok, trim_text(strip_ansi(output), 1200))
    _CMAKE_GENERATOR_PROBE_CACHE[key] = result
    return result


def _ordered_visual_studio_generators() -> list[tuple[str, str]]:
    preferred = [("Visual Studio 17 2022", "x64"), ("Visual Studio 16 2019", "x64")]
    vcvars_path = (get_preferred_vcvars_path() or "").lower()
    if "2019" in vcvars_path:
        preferred.sort(key=lambda item: 0 if "2019" in item[0] else 1)
    return preferred


def _candidate_cmake_generators() -> list[tuple[str, str | None]]:
    available = list_cmake_generators()
    candidates: list[tuple[str, str | None]] = []

    for generator, platform in _ordered_visual_studio_generators():
        if generator in available:
            candidates.append((generator, platform))

    if "NMake Makefiles" in available and has_msvc_toolchain():
        candidates.append(("NMake Makefiles", None))

    if "Ninja" in available and has_msvc_toolchain() and shutil.which("ninja"):
        candidates.append(("Ninja", None))

    return candidates


def _format_cmake_generator_flags(generator: str, platform: str | None = None) -> str:
    parts = [f' -G "{generator}"']
    if platform:
        parts.append(f" -A {platform}")
    return "".join(parts)


def get_execution_environment_guidance() -> str:
    global _EXECUTION_ENV_GUIDANCE_CACHE
    if _EXECUTION_ENV_GUIDANCE_CACHE is not None:
        return _EXECUTION_ENV_GUIDANCE_CACHE

    lines = ["本机执行环境提示："]
    vcvars_path = get_preferred_vcvars_path()
    if vcvars_path:
        lines.append(f"- agent.py 会自动加载可用的 MSVC 环境：{vcvars_path}")
    else:
        lines.append("- 未检测到可用的 vcvars64.bat；不要假设 cl / nmake / msbuild 已可用。")

    generator_summaries = []
    for generator, platform in _candidate_cmake_generators():
        ok, _ = probe_cmake_generator(generator, platform)
        suffix = f" -A {platform}" if platform else ""
        generator_summaries.append(f"{generator}{suffix}: {'可用' if ok else '不可用'}")
    if generator_summaries:
        lines.append("- 已探测的 CMake 生成器：" + "；".join(generator_summaries))

    preferred_flags = _pick_cmake_generator_flags()
    if preferred_flags:
        lines.append(f"- 自动构建验证优先使用：cmake -S . -B build{preferred_flags}")
    else:
        lines.append("- 当前机器未探测到稳定的显式生成器；需要构建时优先复用 agent.py 已加载的环境。")

    lines.append('- 未探测成功前，不要强行执行 `-G "Visual Studio 17 2022"` 或 `-G "Visual Studio 16 2019"`。')
    lines.append("- 不要硬编码 Visual Studio 安装绝对路径；先探测，再执行。")
    lines.append("- 审查阶段优先依据已有 validation_summary 和仓库内容判断，不要把本机环境缺陷误判成代码问题。")
    _EXECUTION_ENV_GUIDANCE_CACHE = "\n".join(lines)
    return _EXECUTION_ENV_GUIDANCE_CACHE


configure_utf8_console()


# =========================
# 配置
# =========================
TASK_FILE    = "tasks.json"
PRD_FILE     = "prd.txt"
REVIEW_DIR   = "reviews"
MAX_RETRY    = 3
CODEX_MODEL  = "gpt-5.4"
AUTO_COMMIT_AFTER_TASK = True

CMD_TIMEOUT_SEC             = int(os.environ.get("AGENT_CMD_TIMEOUT_SEC", 600))
AUTO_TEST_TIMEOUT_SEC       = int(os.environ.get("AGENT_AUTO_TEST_TIMEOUT_SEC", 300))
CODEX_TOTAL_TIMEOUT_SEC     = int(os.environ.get("AGENT_CODEX_TIMEOUT_SEC", 1800))
CODEX_IDLE_TIMEOUT_SEC      = int(os.environ.get("AGENT_CODEX_IDLE_TIMEOUT_SEC", 600))
REVIEW_TOTAL_TIMEOUT_SEC    = int(os.environ.get("AGENT_REVIEW_TIMEOUT_SEC", 600))
REVIEW_IDLE_TIMEOUT_SEC     = int(os.environ.get("AGENT_REVIEW_IDLE_TIMEOUT_SEC", 600))
TASK_REPLAN_LIMIT           = int(os.environ.get("AGENT_TASK_REPLAN_LIMIT", 2))
TASK_CONTEXT_WINDOW         = int(os.environ.get("AGENT_TASK_CONTEXT_WINDOW", 2))
REPO_CONTEXT_SAMPLE_LIMIT   = int(os.environ.get("AGENT_REPO_CONTEXT_SAMPLE_LIMIT", 12))
REVIEW_DIFF_CHAR_LIMIT      = int(os.environ.get("AGENT_REVIEW_DIFF_CHAR_LIMIT", 12000))
STREAM_POLL_INTERVAL_SEC    = 0.2
STREAM_DRAIN_TIMEOUT_SEC    = 5
PROCESS_STOP_TIMEOUT_SEC    = 10
CODEX_STREAM_OUTPUT         = os.environ.get("AGENT_CODEX_STREAM_OUTPUT", "").strip().lower() in {"1", "true", "yes", "on"}
CODEX_STATUS_INTERVAL_SEC   = int(os.environ.get("AGENT_CODEX_STATUS_INTERVAL_SEC", 30))

AGENT_ARTIFACT_PATHS = {TASK_FILE, PRD_FILE, "codex_last_output.txt", "codex_review_last_output.txt"}
AGENT_ARTIFACT_GLOBS = (f"{REVIEW_DIR}/**", ".agent_restore/**", "__pycache__/**")

CPP_SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".rc")
NATIVE_PROJECT_GLOB_PATTERNS = (
    "CMakeLists.txt", "*.cmake", "*.sln", "*.vcxproj", "*.vcproj",
    "*.props", "*.targets", "*.rc", "*.manifest",
)

# C++/Win32 桌面图标工具专用的 Prompt 指导语
WIN32_PROJECT_GUIDANCE = """
项目专属约束（C++/Win32 桌面图标移动工具）：
- 本项目是全新的 C++/Win32 原生桌面工具，从零开始构建，不存在旧代码需要迁移
- 构建系统：CMake + MSVC（Visual Studio 2019/2022），目标平台 x64，最低系统要求 Windows 10
- 核心技术栈：
    * Win32 API：EnumDisplayMonitors、SystemParametersInfo（SPI_GETWORKAREA）
    * Shell API：SHGetDesktopFolder、IShellView、IFolderView2、LVM_SETITEMPOSITION
    * 托盘：Shell_NotifyIcon、WM_TRAYICON 自定义消息
    * 开机自启：HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run 注册表写入/删除
    * 等待 Explorer 就绪：FindWindow("Progman")、FindWindowEx 获取 SysListView32
- 桌面图标操作说明：
    * "移动图标"是指通过 LVM_SETITEMPOSITION 消息修改 Windows 桌面 ListView 中图标的坐标
    * 不是移动桌面上的文件，不要操作文件系统
    * 副屏坐标需要使用虚拟屏幕坐标系（GetSystemMetrics(SM_XVIRTUALSCREEN) 等）
- 任务验证方式（test_commands）：
    * 构建验证：cmake -S . -B build && cmake --build build --config Release
    * 如果是纯逻辑/头文件任务，test_commands 填 []，在 test_hint 中写清楚人工验收步骤
    * 不要生成 pytest、npm test、flutter analyze 等与本项目无关的验证命令
- 代码风格：
    * 所有源文件使用 UTF-8 with BOM 编码（Windows MSVC 友好）
    * 错误处理：使用 GetLastError() + FormatMessage，关键失败写日志文件
    * 日志输出到 %APPDATA%\\IconMover\\icon_mover.log
"""


# =========================
# 颜色输出
# =========================
def c(text, color):
    codes = {"red": 31, "green": 32, "yellow": 33, "cyan": 36, "bold": 1}
    return f"\033[{codes.get(color, 0)}m{text}\033[0m"


def log(icon, msg, color="bold"):
    ts = datetime.now().strftime("%H:%M:%S")
    print(f"[{ts}] {icon}  {c(msg, color)}")


# =========================
# 通用工具
# =========================
def close_stream(stream):
    if stream is None:
        return
    try:
        stream.close()
    except Exception:
        pass


def terminate_process_tree(process: subprocess.Popen):
    if process.poll() is not None:
        return
    if os.name == "nt":
        try:
            subprocess.run(
                ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                check=False, env=build_cli_env(),
            )
        except Exception:
            pass
    else:
        try:
            process.terminate()
        except Exception:
            pass
    try:
        process.wait(timeout=PROCESS_STOP_TIMEOUT_SEC)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        process.kill()
    except Exception:
        pass
    try:
        process.wait(timeout=PROCESS_STOP_TIMEOUT_SEC)
    except Exception:
        pass


def _stream_reader(stream, output_queue: queue.Queue):
    try:
        while True:
            chunk = os.read(stream.fileno(), 4096)
            if not chunk:
                break
            output_queue.put(("data", chunk.decode("utf-8", errors="replace")))
    except Exception:
        pass
    finally:
        output_queue.put(("eof", None))


def _write_process_input(process: subprocess.Popen, input_text: str):
    try:
        if process.stdin is None:
            return
        process.stdin.write(input_text.encode("utf-8"))
        process.stdin.close()
    except Exception:
        pass


def run_streaming_cmd(
    cmd, *, label, cwd=None, input_text=None, total_timeout=0, idle_timeout=0,
    echo_output=True, status_interval=0,
):
    use_shell = isinstance(cmd, str)
    env = build_cli_env()
    env["CI"] = "true"
    env["CONTINUOUS_INTEGRATION"] = "true"

    process = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE if input_text is not None else subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=False, bufsize=0, cwd=cwd, shell=use_shell, env=env,
    )

    output_queue = queue.Queue()
    reader_thread = threading.Thread(target=_stream_reader, args=(process.stdout, output_queue), daemon=True)
    reader_thread.start()

    writer_thread = None
    if input_text is not None:
        writer_thread = threading.Thread(target=_write_process_input, args=(process, input_text), daemon=True)
        writer_thread.start()

    lines = []
    reader_done = False
    guard_reason = None
    started_at = time.monotonic()
    last_output_at = started_at
    last_status_at = started_at
    exited_at = None

    try:
        while True:
            try:
                kind, payload = output_queue.get(timeout=STREAM_POLL_INTERVAL_SEC)
            except queue.Empty:
                kind, payload = None, None

            now = time.monotonic()

            if kind == "data":
                if echo_output:
                    print(payload, end="", flush=True)
                lines.append(payload)
                last_output_at = now
            elif kind == "eof":
                reader_done = True

            if process.poll() is not None and exited_at is None:
                exited_at = now

            if process.poll() is None:
                if not echo_output and status_interval > 0 and now - last_status_at >= status_interval:
                    log("⏳", f"{label} 运行中，已等待 {int(now - started_at)}s", "cyan")
                    last_status_at = now
                if total_timeout > 0 and now - started_at > total_timeout:
                    guard_reason = f"总超时 {total_timeout}s"
                elif idle_timeout > 0 and now - last_output_at > idle_timeout:
                    guard_reason = f"静默超时 {idle_timeout}s"
                if guard_reason:
                    log("⏱️", f"{label} {guard_reason}，正在强制结束进程", "yellow")
                    terminate_process_tree(process)
                    break
            elif not reader_done and exited_at is not None and now - exited_at > STREAM_DRAIN_TIMEOUT_SEC:
                guard_reason = "子进程已退出但 stdout 未关闭"
                close_stream(process.stdout)
                break

            if process.poll() is not None and reader_done:
                break
    except KeyboardInterrupt:
        log("⏹️", f"检测到中断，正在停止 {label} 子进程", "yellow")
        terminate_process_tree(process)
        raise
    finally:
        close_stream(process.stdin)
        try:
            process.wait(timeout=PROCESS_STOP_TIMEOUT_SEC)
        except subprocess.TimeoutExpired:
            terminate_process_tree(process)

        drain_deadline = time.monotonic() + STREAM_DRAIN_TIMEOUT_SEC
        while time.monotonic() < drain_deadline:
            drained = False
            while True:
                try:
                    kind, payload = output_queue.get_nowait()
                except queue.Empty:
                    break
                drained = True
                if kind == "data":
                    if echo_output:
                        print(payload, end="", flush=True)
                    lines.append(payload)
                elif kind == "eof":
                    reader_done = True
            if reader_done or (not drained and process.poll() is not None):
                break
            time.sleep(STREAM_POLL_INTERVAL_SEC)

        close_stream(process.stdout)
        reader_thread.join(timeout=1)
        if writer_thread is not None:
            writer_thread.join(timeout=1)

    return {"returncode": process.returncode, "output": "".join(lines).strip(), "guard_reason": guard_reason}


def run_cmd_capture(cmd, cwd=None, timeout=CMD_TIMEOUT_SEC):
    use_shell = isinstance(cmd, str)
    process = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, encoding="utf-8", errors="replace",
        cwd=cwd, shell=use_shell, env=build_cli_env(),
    )
    effective_timeout = timeout if timeout and timeout > 0 else None
    try:
        stdout, stderr = process.communicate(timeout=effective_timeout)
    except subprocess.TimeoutExpired:
        terminate_process_tree(process)
        stdout, stderr = process.communicate()
        timeout_note = f"[命令执行超时: {timeout}s]\n"
        return False, stdout, f"{timeout_note}{stderr}".strip()
    except KeyboardInterrupt:
        terminate_process_tree(process)
        raise
    return process.returncode == 0, stdout, stderr


def run_cmd(cmd, cwd=None, timeout=CMD_TIMEOUT_SEC):
    ok, stdout, stderr = run_cmd_capture(cmd, cwd=cwd, timeout=timeout)
    return ok, stdout + stderr


def resolve_cli(name: str) -> str:
    return shutil.which(name) or name


def normalize_codex_prompt(prompt: str) -> str:
    return prompt.rstrip() + "\n\n补充要求：直接输出本次请求的最终结果，不要追加『如果你愿意我可以继续』等征询式收尾。"


def write_temp_json_file(prefix: str, payload) -> Path:
    fd, temp_path = tempfile.mkstemp(prefix=prefix, suffix=".json")
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2, ensure_ascii=False)
    return Path(temp_path)


def save_tasks(tasks: list[dict]):
    with open(TASK_FILE, "w", encoding="utf-8") as fh:
        json.dump(tasks, fh, indent=2, ensure_ascii=False)


def strip_ansi(text: str) -> str:
    return re.sub(r"\x1b\[[0-9;]*m", "", text or "")


def trim_text(text: str, limit: int = 4000) -> str:
    text = (text or "").strip()
    if len(text) <= limit:
        return text
    return text[:limit - 3].rstrip() + "..."


def dedupe_keep_order(items: list[str]) -> list[str]:
    seen = set()
    ordered = []
    for item in items:
        if item in seen:
            continue
        seen.add(item)
        ordered.append(item)
    return ordered


def extract_json_payload(raw: str, expected_type=None):
    raw = (raw or "").strip()
    if not raw:
        raise ValueError("空输出，无法解析 JSON")
    candidates = [raw]
    for start_char, end_char in (("[", "]"), ("{", "}")):
        start = raw.find(start_char)
        end = raw.rfind(end_char)
        if start != -1 and end > start:
            candidates.append(raw[start:end + 1])
    last_error = None
    for candidate in candidates:
        try:
            parsed = json.loads(candidate)
            if expected_type and not isinstance(parsed, expected_type):
                raise ValueError(f"JSON 类型不是 {expected_type}")
            return parsed
        except Exception as exc:
            last_error = exc
    raise ValueError(f"JSON 解析失败: {last_error}")


CODEX_TASKS_SCHEMA = {
    "type": "array",
    "items": {
        "type": "object",
        "additionalProperties": False,
        "required": ["id", "phase", "desc", "depends_on", "files_hint", "test_hint", "test_commands"],
        "properties": {
            "id": {"type": "string"},
            "phase": {"type": "string"},
            "desc": {"type": "string"},
            "depends_on": {"type": "array", "items": {"type": "string"}},
            "files_hint": {"type": "array", "items": {"type": "string"}},
            "test_hint": {"type": "string"},
            "test_commands": {"type": "array", "items": {"type": "string"}},
        },
    },
}


CODEX_REVIEW_SCHEMA = {
    "type": "object",
    "additionalProperties": False,
    "required": ["result", "summary"],
    "properties": {
        "result": {"type": "string", "enum": ["PASS", "FAIL"]},
        "summary": {"type": "string"},
    },
}


CODEX_REPLAN_TASK_SCHEMA = {
    "type": "object",
    "additionalProperties": False,
    "required": ["desc", "files_hint", "test_hint", "test_commands"],
    "properties": {
        "desc": {"type": "string"},
        "files_hint": {"type": "array", "items": {"type": "string"}},
        "test_hint": {"type": "string"},
        "test_commands": {"type": "array", "items": {"type": "string"}},
    },
}


CODEX_REPLAN_SCHEMA = {
    "type": "object",
    "additionalProperties": False,
    "required": ["action", "reason", "new_tasks", "updated_task"],
    "properties": {
        "action": {"type": "string", "enum": ["replan", "update"]},
        "reason": {"type": "string"},
        "new_tasks": {
            "type": "array",
            "items": {
                "type": "object",
                "additionalProperties": False,
                "required": ["id", "phase", "desc", "depends_on", "files_hint", "test_hint", "test_commands"],
                "properties": {
                    "id": {"type": "string"},
                    "phase": {"type": "string"},
                    "desc": {"type": "string"},
                    "depends_on": {"type": "array", "items": {"type": "string"}},
                    "files_hint": {"type": "array", "items": {"type": "string"}},
                    "test_hint": {"type": "string"},
                    "test_commands": {"type": "array", "items": {"type": "string"}},
                },
            },
        },
        "updated_task": {
            "anyOf": [
                {"type": "null"},
                CODEX_REPLAN_TASK_SCHEMA,
            ]
        },
    },
}


def format_review_payload(review_payload: dict) -> str:
    result = str(review_payload.get("result") or "").strip().upper() or "FAIL"
    summary = str(review_payload.get("summary") or "").strip()
    return result if not summary else f"{result}\n{summary}"


# =========================
# 仓库上下文
# =========================
_repo_context_cache = None


def get_repo_context() -> str:
    global _repo_context_cache
    if _repo_context_cache is not None:
        return _repo_context_cache

    root = Path(".")
    top_entries = sorted(root.iterdir(), key=lambda p: p.name.lower())
    top_summary = "、".join(entry.name for entry in top_entries[:12])
    lines = [
        f"仓库根目录：{Path.cwd().name}",
        f"顶层目录/文件示例：{top_summary}",
    ]

    # 检测 CMake/MSVC 工程文件
    cmake_files = [str(p) for p in sorted(root.rglob("CMakeLists.txt")) if p.is_file()][:6]
    if cmake_files:
        lines.append(f"CMakeLists.txt：{'、'.join(cmake_files)}")

    sln_files = [str(p) for p in sorted(root.rglob("*.sln")) if p.is_file()][:4]
    if sln_files:
        lines.append(f"Visual Studio 解决方案：{'、'.join(sln_files)}")

    # 采样 C++ 源文件
    cpp_files = []
    for path in sorted(root.rglob("*")):
        if path.is_file() and path.suffix.lower() in CPP_SOURCE_SUFFIXES:
            cpp_files.append(path.as_posix())
            if len(cpp_files) >= REPO_CONTEXT_SAMPLE_LIMIT:
                break
    if cpp_files:
        lines.append(f"C++ 源文件示例：{'、'.join(cpp_files)}")

    _repo_context_cache = "\n".join(lines)
    return _repo_context_cache


def get_task_neighbor_context(tasks: list[dict], task_id: str, window: int = TASK_CONTEXT_WINDOW) -> str:
    for index, task in enumerate(tasks):
        if task["id"] != task_id:
            continue
        start = max(0, index - window)
        end = min(len(tasks), index + window + 1)
        snippet = []
        for pos in range(start, end):
            item = tasks[pos]
            marker = "当前" if item["id"] == task_id else f"{pos + 1:02d}"
            snippet.append(f"- {marker} [{item.get('status', 'todo')}] {item['id']}: {item['desc']}")
        return "\n".join(snippet)
    return ""


# =========================
# Codex 调用
# =========================
def call_codex(
    prompt: str,
    *,
    label: str = "Codex",
    output_file_name: str = "codex_last_output.txt",
    total_timeout: int | None = None,
    idle_timeout: int | None = None,
    output_schema: dict | None = None,
    failure_fallback: str = "",
) -> str:
    output_file = Path(output_file_name)
    if output_file.exists():
        output_file.unlink()

    icon = "🔍" if "Review" in label else "🤖"
    log(icon, f"调用 {label}（简洁模式）...", "cyan")

    schema_file = None
    try:
        prompt = normalize_codex_prompt(prompt)
        cmd = [
            resolve_cli("codex"), "exec",
            "-m", CODEX_MODEL,
            "-s", "danger-full-access",
            "--dangerously-bypass-approvals-and-sandbox",
            "--ephemeral",
            "--color", "never",
            "-o", str(output_file),
        ]
        if output_schema:
            schema_file = write_temp_json_file("codex-output-schema-", output_schema)
            cmd.extend(["--output-schema", str(schema_file)])
        cmd.append("-")

        result = run_streaming_cmd(
            cmd,
            label=label,
            input_text=prompt,
            total_timeout=CODEX_TOTAL_TIMEOUT_SEC if total_timeout is None else total_timeout,
            idle_timeout=CODEX_IDLE_TIMEOUT_SEC if idle_timeout is None else idle_timeout,
            echo_output=CODEX_STREAM_OUTPUT,
            status_interval=0 if CODEX_STREAM_OUTPUT else CODEX_STATUS_INTERVAL_SEC,
        )

        if output_file.exists():
            output = output_file.read_text(encoding="utf-8").strip()
            output_file.unlink()
        else:
            output = result["output"]

        if result["guard_reason"] and output:
            log("⚠️", f"{label} 触发进程保护（{result['guard_reason']}），已使用最终输出继续", "yellow")
        elif result["guard_reason"] and not output:
            log("❌", f"{label} 触发进程保护（{result['guard_reason']}），且未拿到最终输出", "red")

        if result["returncode"] != 0 and not output:
            stream_text = result["output"]
            if "stream disconnected before completion" in stream_text:
                log("❌", f"{label} 连接在响应完成前中断，请检查网络或代理配置。", "red")
            else:
                log("❌", f"{label} 调用失败 (exit {result['returncode']})", "red")
            return failure_fallback

        return output
    finally:
        output_file.unlink(missing_ok=True)
        if schema_file is not None:
            schema_file.unlink(missing_ok=True)


def call_codex_review(prompt: str, *, output_schema: dict | None = None) -> str:
    return call_codex(
        prompt,
        label="Codex-Review",
        output_file_name="codex_review_last_output.txt",
        total_timeout=REVIEW_TOTAL_TIMEOUT_SEC,
        idle_timeout=REVIEW_IDLE_TIMEOUT_SEC,
        output_schema=output_schema,
        failure_fallback="[Codex审核调用失败]\nFAIL",
    )


# =========================
# Git 管理
# =========================
def normalize_repo_path(path: str) -> str:
    return path.strip().replace("\\", "/")


def sanitize_repo_path_candidate(path: str) -> str | None:
    normalized = normalize_repo_path(path)
    if not normalized:
        return None

    lowered = normalized.lower()
    if lowered.startswith(("warning:", "error:", "fatal:", "hint:", "note:")):
        return None

    if normalized.startswith("/") or normalized.startswith("../"):
        return None

    parts = normalized.split("/")
    invalid_chars = '<>:"|?*'
    for part in parts:
        if not part or part in (".", ".."):
            return None
        if any(ch in part for ch in invalid_chars):
            return None
    return normalized


def parse_nul_delimited_paths(raw: str) -> list[str]:
    paths = []
    for item in raw.split("\0"):
        normalized = sanitize_repo_path_candidate(item)
        if normalized:
            paths.append(normalized)
    return paths


def list_tracked_change_files() -> list[str]:
    changed = set()
    for cmd in (
        ["git", "diff", "--name-only", "-z"],
        ["git", "diff", "--cached", "--name-only", "-z"],
    ):
        ok, stdout, _stderr = run_cmd_capture(cmd)
        if not ok and not stdout:
            continue
        changed.update(parse_nul_delimited_paths(stdout))
    return sorted(changed)


def list_untracked_files() -> list[str]:
    ok, stdout, _stderr = run_cmd_capture(["git", "ls-files", "-z", "--others", "--exclude-standard"])
    if not ok:
        return []
    return parse_nul_delimited_paths(stdout)


def is_agent_artifact_path(path: str) -> bool:
    normalized = normalize_repo_path(path)
    if not normalized:
        return False
    if normalized in AGENT_ARTIFACT_PATHS:
        return True
    return any(
        normalized == prefix[:-3] or normalized.startswith(prefix[:-2])
        for prefix in AGENT_ARTIFACT_GLOBS
    )


def list_non_artifact_workspace_changes() -> list[str]:
    dirty_paths = set(list_tracked_change_files()) | set(list_untracked_files())
    return sorted(path for path in dirty_paths if not is_agent_artifact_path(path))


def snapshot_workspace_paths(snapshot_dir: Path, paths: list[str]) -> dict[str, dict]:
    manifest: dict[str, dict] = {}
    snapshot_dir.mkdir(parents=True, exist_ok=True)

    for raw_path in paths:
        normalized = sanitize_repo_path_candidate(raw_path)
        if not normalized:
            continue

        source_path = Path(normalized)
        entry = {"exists": source_path.exists()}
        manifest[normalized] = entry

        if not entry["exists"]:
            continue

        target_path = snapshot_dir / Path(normalized)
        target_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_path, target_path)

    return manifest


def get_snapshot_path(restore_point: dict, path: str) -> Path:
    return Path(restore_point["snapshot_dir"]) / Path(normalize_repo_path(path))


def path_matches_snapshot(path: str, restore_point: dict) -> bool:
    normalized = normalize_repo_path(path)
    entry = restore_point.get("baseline_manifest", {}).get(normalized)
    if entry is None:
        return False

    current_path = Path(normalized)
    current_exists = current_path.exists()
    if current_exists != bool(entry.get("exists")):
        return False
    if not current_exists:
        return True

    snapshot_path = get_snapshot_path(restore_point, normalized)
    if not snapshot_path.exists():
        return False
    return current_path.read_bytes() == snapshot_path.read_bytes()


def list_task_change_paths(restore_point: dict) -> list[str]:
    changed_paths = []
    for path in list_non_artifact_workspace_changes():
        if path_matches_snapshot(path, restore_point):
            continue
        changed_paths.append(path)
    return sorted(set(changed_paths))


def read_head_file_bytes(path: str) -> bytes | None:
    result = subprocess.run(
        ["git", "show", f"HEAD:{normalize_repo_path(path)}"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        env=build_cli_env(),
    )
    if result.returncode != 0:
        return None
    return result.stdout


def read_current_file_bytes(path: str) -> bytes | None:
    file_path = Path(normalize_repo_path(path))
    if not file_path.exists() or file_path.is_dir():
        return None
    return file_path.read_bytes()


def read_baseline_file_bytes(path: str, restore_point: dict) -> bytes | None:
    normalized = normalize_repo_path(path)
    entry = restore_point.get("baseline_manifest", {}).get(normalized)
    if entry is not None:
        if not entry.get("exists"):
            return None
        snapshot_path = get_snapshot_path(restore_point, normalized)
        if not snapshot_path.exists():
            return None
        return snapshot_path.read_bytes()
    return read_head_file_bytes(normalized)


def is_probably_text_bytes(data: bytes | None) -> bool:
    if data is None:
        return True
    return b"\x00" not in data


def decode_diff_lines(data: bytes | None) -> list[str]:
    if data is None:
        return []
    return data.decode("utf-8", errors="replace").splitlines()


def build_task_diff(path: str, restore_point: dict) -> str:
    normalized = normalize_repo_path(path)
    baseline_bytes = read_baseline_file_bytes(normalized, restore_point)
    current_bytes = read_current_file_bytes(normalized)

    if baseline_bytes == current_bytes:
        return ""

    if not is_probably_text_bytes(baseline_bytes) or not is_probably_text_bytes(current_bytes):
        change_type = "modified"
        if baseline_bytes is None:
            change_type = "new file"
        elif current_bytes is None:
            change_type = "deleted file"
        return (
            f"diff --task a/{normalized} b/{normalized}\n"
            f"Binary or non-text content changed ({change_type})\n"
        )

    diff_lines = difflib.unified_diff(
        decode_diff_lines(baseline_bytes),
        decode_diff_lines(current_bytes),
        fromfile=f"a/{normalized}",
        tofile=f"b/{normalized}",
        lineterm="",
    )
    return "\n".join(diff_lines)


def restore_baseline_workspace(restore_point: dict):
    manifest = restore_point.get("baseline_manifest", {})
    for raw_path in sorted(manifest):
        normalized = sanitize_repo_path_candidate(raw_path)
        if not normalized:
            continue

        target_path = Path(normalized)
        entry = manifest[raw_path]

        if entry.get("exists"):
            snapshot_path = get_snapshot_path(restore_point, normalized)
            if not snapshot_path.exists():
                raise RuntimeError(f"恢复快照缺失: {normalized}")
            if target_path.exists() and target_path.is_dir() and not target_path.is_symlink():
                shutil.rmtree(target_path, ignore_errors=True)
            target_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(snapshot_path, target_path)
            continue

        if target_path.is_dir() and not target_path.is_symlink():
            shutil.rmtree(target_path, ignore_errors=True)
        else:
            target_path.unlink(missing_ok=True)


def classify_commit_type(desc: str) -> str:
    rules = (
        ("fix",      ("修复", "修正", "解决", "兼容", "错误", "异常", "bug")),
        ("refactor", ("重构", "抽离", "拆分", "整理", "优化结构")),
        ("test",     ("测试", "test", "验证")),
        ("docs",     ("文档", "README", "说明", "注释")),
        ("build",    ("构建", "cmake", "打包", "发布", "CI")),
        ("chore",    ("配置", "依赖", "初始化", "工程化")),
    )
    lowered = desc.lower()
    for commit_type, keywords in rules:
        if any(keyword.lower() in lowered for keyword in keywords):
            return commit_type
    return "feat"


def build_commit_message(task: dict, changed_paths: list[str]) -> tuple[str, str]:
    summary = " ".join(task["desc"].replace("\n", " ").replace("`", "").split()).strip(" .。；;，,")
    summary = re.sub(r"\s+", " ", summary)
    summary = f"{task['id']} {summary}"
    if len(summary) > 72:
        summary = summary[:69].rstrip() + "..."
    commit_type = classify_commit_type(task["desc"])
    subject = f"{commit_type}: {summary}"
    body_lines = [
        f"Task: {task['id']}",
        f"Description: {task['desc']}",
    ]
    test_hint = task.get("test_hint")
    if test_hint:
        body_lines.append(f"Validation: {test_hint}")
    body_lines.append("Files:")
    for path in (changed_paths or [])[:10]:
        body_lines.append(f"  {path}")
    return subject, "\n".join(body_lines)


def git_commit_task(task: dict, restore_point: dict):
    if not AUTO_COMMIT_AFTER_TASK:
        return
    changed_paths = list_task_change_paths(restore_point)
    if not changed_paths:
        log("💡", "本次任务没有代码改动，跳过提交", "yellow")
        return
    subject, body = build_commit_message(task, changed_paths)
    ok, out = run_cmd(["git", "add", "-A", "--", *changed_paths])
    if not ok:
        log("⚠️", f"git add 失败: {out[:200]}", "yellow")
        return
    ok, out = run_cmd(["git", "commit", "--only", "-m", subject, "-m", body, "--", *changed_paths])
    if ok:
        log("📦", f"已提交: {subject}", "green")
    else:
        log("⚠️", f"git commit 失败: {out[:200]}", "yellow")


def create_restore_point(task_id: str) -> dict:
    restore_dir = Path(".agent_restore") / task_id
    restore_dir.mkdir(parents=True, exist_ok=True)

    ok, out = run_cmd(["git", "rev-parse", "HEAD"])
    tracked_source = out.strip() if ok and out.strip() else None

    preexisting_dirty_paths = list_non_artifact_workspace_changes()
    baseline_untracked_files = [
        path for path in list_untracked_files()
        if not is_agent_artifact_path(path)
    ]
    snapshot_dir = restore_dir / "baseline"
    baseline_manifest = snapshot_workspace_paths(snapshot_dir, preexisting_dirty_paths)
    return {
        "dir": str(restore_dir),
        "tracked_source": tracked_source,
        "preexisting_dirty_paths": preexisting_dirty_paths,
        "baseline_untracked_files": baseline_untracked_files,
        "snapshot_dir": str(snapshot_dir),
        "baseline_manifest": baseline_manifest,
    }


def cleanup_restore_point(restore_point: dict | None):
    if not restore_point:
        return
    shutil.rmtree(restore_point["dir"], ignore_errors=True)


def git_reset(restore_point: dict):
    source = restore_point.get("tracked_source")
    if source:
        ok, out = run_cmd(["git", "restore", f"--source={source}", "--staged", "--worktree", "--", "."])
        if not ok:
            raise RuntimeError(f"恢复 tracked 文件失败:\n{out}")

    baseline_untracked = set(restore_point.get("baseline_untracked_files", []))
    current_untracked = {
        path for path in list_untracked_files()
        if not is_agent_artifact_path(path)
    }
    for rel_path in sorted(current_untracked - baseline_untracked, reverse=True):
        normalized = sanitize_repo_path_candidate(rel_path)
        if not normalized:
            continue

        path = Path(normalized)
        if path.is_dir() and not path.is_symlink():
            shutil.rmtree(path, ignore_errors=True)
        else:
            path.unlink(missing_ok=True)

    restore_baseline_workspace(restore_point)
    log("🔄", "已恢复到当前任务开始前的状态", "yellow")


def git_diff_since(restore_point: dict) -> str:
    changed_paths = list_task_change_paths(restore_point)
    if not changed_paths:
        return "(无 diff)"

    parts = []
    for path in changed_paths:
        diff = build_task_diff(path, restore_point)
        if diff:
            parts.append(diff)
    return "\n".join(parts) if parts else "(无 diff)"


# =========================
# 任务数据规范化
# =========================
def _normalize_str_list(value) -> list[str]:
    if value in (None, ""):
        return []
    if isinstance(value, list):
        return [str(item).strip() for item in value if str(item).strip()]
    return [str(value).strip()]


def normalize_task_record(task: dict, idx: int, *, default_id: str | None = None) -> dict:
    if not isinstance(task, dict):
        raise ValueError(f"第 {idx} 个任务不是对象")
    task_id = str(task.get("id") or default_id or f"task_{idx:03d}")
    desc = str(task.get("desc") or "").strip()
    if not desc:
        raise ValueError(f"第 {idx} 个任务缺少 desc")
    return {
        **task,
        "id": task_id,
        "phase": str(task.get("phase") or "").strip(),
        "desc": desc,
        "depends_on": _normalize_str_list(task.get("depends_on")),
        "files_hint": _normalize_str_list(task.get("files_hint")),
        "test_hint": str(task.get("test_hint") or "构建成功，功能正常运行").strip(),
        "test_commands": _normalize_str_list(task.get("test_commands")),
        "status": str(task.get("status") or "todo"),
        "retries": int(task.get("retries") or 0),
        "replans": int(task.get("replans") or 0),
        "review_result": task.get("review_result"),
        "blocked_by": _normalize_str_list(task.get("blocked_by")),
        "last_error": str(task.get("last_error") or "").strip(),
        "diagnosis": str(task.get("diagnosis") or "").strip(),
    }


def normalize_tasks(tasks: list) -> list:
    if not isinstance(tasks, list):
        raise ValueError("任务文件必须是 JSON 数组")
    return [normalize_task_record(task, idx) for idx, task in enumerate(tasks, 1)]


def next_task_number(tasks: list[dict]) -> tuple[int, int]:
    max_num = 0
    width = 3
    for task in tasks:
        match = re.search(r"(\d+)$", str(task.get("id", "")))
        if not match:
            continue
        max_num = max(max_num, int(match.group(1)))
        width = max(width, len(match.group(1)))
    return max_num + 1, width


def normalize_new_tasks(raw_tasks: list, existing_tasks: list[dict]) -> list[dict]:
    next_num, width = next_task_number(existing_tasks)
    normalized = []
    for offset, raw_task in enumerate(raw_tasks):
        task_id = f"task_{next_num + offset:0{width}d}"
        normalized.append(normalize_task_record(raw_task, offset + 1, default_id=task_id))
    return normalized


def build_task_lookup(tasks: list[dict]) -> dict[str, dict]:
    return {task["id"]: task for task in tasks}


def unresolved_dependencies(task: dict, tasks: list[dict], terminal_failed_ids: set[str] | None = None) -> list[str]:
    lookup = build_task_lookup(tasks)
    blockers = []
    terminal_failed_ids = terminal_failed_ids or set()
    for dep_id in task.get("depends_on", []):
        dep_task = lookup.get(dep_id)
        if dep_task is None or dep_task["id"] in terminal_failed_ids:
            blockers.append(dep_id)
            continue
        if dep_task.get("status") != "done":
            blockers.append(dep_id)
    return blockers


def stable_topological_sort(tasks: list[dict]) -> list[dict]:
    if not any(task.get("depends_on") for task in tasks):
        return tasks
    lookup = build_task_lookup(tasks)
    indegree = {task["id"]: 0 for task in tasks}
    edges = {task["id"]: [] for task in tasks}
    order = {task["id"]: idx for idx, task in enumerate(tasks)}
    for task in tasks:
        for dep_id in task.get("depends_on", []):
            if dep_id not in lookup:
                continue
            indegree[task["id"]] += 1
            edges[dep_id].append(task["id"])
    ready = sorted([tid for tid, deg in indegree.items() if deg == 0], key=lambda tid: order[tid])
    result_ids = []
    while ready:
        tid = ready.pop(0)
        result_ids.append(tid)
        for neighbor in sorted(edges[tid], key=lambda item: order[item]):
            indegree[neighbor] -= 1
            if indegree[neighbor] == 0:
                ready.append(neighbor)
                ready.sort(key=lambda item: order[item])
    if len(result_ids) != len(tasks):
        log("⚠️", "检测到任务依赖存在环或非法引用，保留原始任务顺序", "yellow")
        return tasks
    return [lookup[tid] for tid in result_ids]


# =========================
# C++ 构建验证（仅 CMake/MSVC）
# =========================
def _pick_cmake_generator_flags() -> str:
    """
    在 Windows 上为 cmake -S ... -B ... 选择合适的 -G 生成器参数。
    优先选择经过本机探测确认为可用的生成器，避免只靠路径猜测
    Visual Studio 版本后误判当前环境。
    """
    for generator, platform in _candidate_cmake_generators():
        ok, _ = probe_cmake_generator(generator, platform)
        if ok:
            return _format_cmake_generator_flags(generator, platform)
    return ""


def infer_cpp_validation_commands(task: dict | None = None) -> list[str]:
    """仅针对 C++/CMake/MSVC 项目推断构建验证命令。"""
    # 优先使用任务自身指定的命令
    explicit = dedupe_keep_order(_normalize_str_list((task or {}).get("test_commands")))
    if explicit:
        return explicit

    base = Path(".")
    cmake_files = [p for p in sorted(base.rglob("CMakeLists.txt")) if p.is_file()]
    if cmake_files:
        source_root = cmake_files[0].parent
        try:
            rel = source_root.relative_to(base)
        except ValueError:
            rel = source_root
        source_token = rel.as_posix() if str(rel) not in ("", ".") else "."
        build_dir = "build" if source_token == "." else f"{source_token}/build"
        # 在 Windows 上显式指定生成器，避免默认落到 NMake Makefiles
        generator_flags = _pick_cmake_generator_flags() if os.name == "nt" else ""
        configure_cmd = f"cmake -S {source_token} -B {build_dir}{generator_flags}"
        return [
            configure_cmd,
            f"cmake --build {build_dir} --config Release",
        ]

    sln_files = [p for p in sorted(base.rglob("*.sln")) if p.is_file()]
    if sln_files:
        try:
            sln = sln_files[0].relative_to(base)
        except ValueError:
            sln = sln_files[0]
        return [f'msbuild "{sln}" /m /p:Configuration=Release /p:Platform=x64']

    # 仓库为空（项目刚初始化），返回空，待 CMakeLists.txt 创建后再验证
    return []


def auto_test(task: dict | None = None) -> tuple[bool, str]:
    """执行 C++ 构建验证，返回 (success, output)。"""
    commands = infer_cpp_validation_commands(task)
    if not commands:
        msg = "暂无可执行的构建验证命令（CMakeLists.txt 尚未创建），跳过构建验证。"
        log("💡", msg, "yellow")
        return True, msg

    outputs = []
    for command in commands:
        outputs.append(f"$ {command}")
        ok, output = run_cmd(command, timeout=AUTO_TEST_TIMEOUT_SEC)
        outputs.append(trim_text(output, 8000))
        if not ok:
            combined = "\n\n".join(outputs).strip()
            return False, combined

    return True, "\n\n".join(outputs).strip()


# =========================
# Step 1: 需求 → PRD
# =========================
def requirement_to_prd(requirement: str) -> str:
    log("📋", "Codex 生成 PRD ...", "cyan")
    repo_context = get_repo_context()

    prompt = f"""
你是资深产品经理和 Windows 桌面软件架构师。

请将以下用户需求扩写为完整的 PRD（产品需求文档）。

{WIN32_PROJECT_GUIDANCE}

要求：
1. 用中文输出
2. 这是一个全新的 C++/Win32 项目，从零开始构建
3. 至少包含：背景、目标、范围、非目标、功能列表、技术架构、关键 Win32 API 说明、阶段性验收标准、约束条件、风险与降级方案
4. 功能列表要详细且可执行，明确每个功能对应的 Win32 API 和实现思路
5. 不要输出任何代码，只输出 PRD 文档

当前仓库上下文：
{repo_context}

用户需求：
{requirement}
"""
    prd = call_codex(prompt, label="Codex-PRD")
    Path(PRD_FILE).write_text(prd, encoding="utf-8")
    log("✅", f"PRD 已写入 {PRD_FILE}", "green")
    print("\n" + c("─" * 60, "cyan"))
    print(prd[:1500] + ("..." if len(prd) > 1500 else ""))
    print(c("─" * 60, "cyan") + "\n")
    return prd


# =========================
# Step 2: PRD → Tasks
# =========================
def prd_to_tasks(prd: str) -> list:
    log("🗂️", "Codex 拆分任务 ...", "cyan")
    repo_context = get_repo_context()

    prompt = f"""
你是 Windows 桌面软件架构师。

根据以下 PRD 拆分为最小可执行的开发任务列表。

{WIN32_PROJECT_GUIDANCE}

拆分要求：
1. 每个任务独立可完成，粒度适中（约30分钟~2小时工作量）
2. 顺序：工程初始化/CMake配置 → 核心数据结构 → Win32 基础能力 → 托盘/UI → 业务功能闭环 → 收尾与测试
3. 不要把互相依赖的内容拆反
4. 只输出合法 JSON 数组，不要有任何其他文字或 markdown
5. 每个任务包含字段：
   - id: 字符串，如 "task_001"
   - phase: 阶段名，如 "bootstrap" / "foundation" / "feature" / "quality"
   - desc: 任务描述（中文，说清楚要实现什么，涉及哪些 Win32 API）
   - depends_on: 前置任务 id 数组，没有则为空数组
   - files_hint: 预计涉及的文件或目录（可为空数组）
   - test_hint: 验收标准，如果无法自动验证请写清楚人工验收步骤
   - test_commands: 可执行验证命令数组，优先使用 cmake/msbuild；纯逻辑任务填 []
6. depends_on 只能引用当前数组中更早出现的任务 id，不要产生环依赖

当前仓库上下文：
{repo_context}

PRD：
{prd}
"""
    raw = call_codex(prompt, label="Codex-Tasks", output_schema=CODEX_TASKS_SCHEMA)
    try:
        tasks = extract_json_payload(raw, list)
    except ValueError:
        log("❌", "任务解析失败，请检查 Codex 输出", "red")
        print("原始输出：\n", raw)
        sys.exit(1)

    tasks = stable_topological_sort(normalize_tasks(tasks))
    save_tasks(tasks)
    log("✅", f"共 {len(tasks)} 个任务已写入 {TASK_FILE}", "green")
    for t in tasks:
        phase = f"{t['phase']} | " if t.get("phase") else ""
        deps = f" <- {','.join(t.get('depends_on', []))}" if t.get("depends_on") else ""
        print(f"  [{t['id']}] {phase}{t['desc']}{deps}")
    print()
    return tasks


# =========================
# Step 3: 执行任务
# =========================
def execute_task(task: dict, *, tasks: list[dict], error: str = None):
    files_hint = "、".join(task.get("files_hint", [])) or "自动判断"
    depends_on = "、".join(task.get("depends_on", [])) or "无"
    phase = task.get("phase") or "未指定"
    repo_context = get_repo_context()
    neighbor_context = get_task_neighbor_context(tasks, task["id"])
    env_guidance = get_execution_environment_guidance()

    prompt = f"""
你是高级 Windows 桌面软件工程师，正在开发一个 C++/Win32 桌面图标移动工具。

{WIN32_PROJECT_GUIDANCE}

当前仓库上下文：
{repo_context}

当前任务：
- ID：{task['id']}
- 阶段：{phase}
- 描述：{task['desc']}
- 前置任务：{depends_on}
- 预计文件：{files_hint}
- 验收标准：{task.get('test_hint', '构建成功，功能正常运行')}
- 验证命令：{task.get('test_commands') or []}

相邻任务上下文：
{neighbor_context or "(无)"}

{env_guidance}

执行规则：
1. 只修改与当前任务强相关的文件，允许补齐必需的最小支撑代码
2. 不删除与本任务无关的已有代码
3. 优先实现可继续推进的最小闭环
4. 修改后自行运行必要命令并修复明显编译报错
5. 保持代码风格一致（UTF-8 BOM、Win32 命名规范）
6. 对 MSVC/CMake 相关命令，优先复用 agent.py 已自动加载的构建环境；在未经探测确认前，不要硬编码 Visual Studio 安装路径，也不要强行指定 Visual Studio CMake generator。
"""
    if error:
        prompt += f"""

上一次尝试失败，错误信息如下，请修复：
{error}
"""
    call_codex(prompt, label="Codex-Exec")


# =========================
# Step 4: 代码审核
# =========================
def codex_review(task: dict, diff: str, validation_summary: str = "") -> tuple[bool, str]:
    diff_snippet = trim_text(diff, REVIEW_DIFF_CHAR_LIMIT)
    validation_snippet = trim_text(validation_summary, 2000)
    env_guidance = get_execution_environment_guidance()

    prompt = f"""
你是资深 Windows 桌面软件代码审核员。

{WIN32_PROJECT_GUIDANCE}

请审核以下代码变更，判断是否符合本次任务要求。

任务：{task['desc']}
验收标准：{task.get('test_hint', '')}

构建验证结果：
{validation_snippet or "(未执行构建验证)"}

代码 diff：
{diff_snippet}

{env_guidance}

审核要点（C++/Win32 专项）：
1. Win32 API 使用是否正确（句柄泄漏、错误码处理、UNICODE/ANSI 版本）
2. 图标坐标操作是否使用了正确的虚拟屏幕坐标系
3. 是否有明显的内存泄漏或资源未释放（HICON、HMENU、窗口句柄等）
4. CMakeLists.txt 配置是否正确（目标平台、链接库、Unicode 宏）
5. 开机自启注册表操作是否正确且可逆
6. 代码整体能否正常编译运行
7. 构建验证以上述 validation_summary 为优先依据；如需补充本机命令验证，必须先按环境提示确认 generator 可用，不要把本机 Visual Studio 实例发现失败误判成代码缺陷。

输出格式（必须以其中一个开头）：
PASS
或
FAIL
<具体问题和修改建议>
"""
    output = call_codex_review(prompt, output_schema=CODEX_REVIEW_SCHEMA)

    try:
        review_payload = extract_json_payload(output, dict)
        report_text = format_review_payload(review_payload)
        passed = str(review_payload.get("result") or "").strip().upper() == "PASS"
    except ValueError:
        report_text = output
        passed = strip_ansi(output).strip().upper().startswith("PASS")

    # 保存审核报告
    Path(REVIEW_DIR).mkdir(exist_ok=True)
    report_path = Path(REVIEW_DIR) / f"{task['id']}_review.txt"
    report_path.write_text(report_text, encoding="utf-8")

    return passed, report_text


# =========================
# 自动重规划
# =========================
def diagnose_and_replan_task(task: dict, tasks: list[dict], error: str) -> bool:
    if task.get("replans", 0) >= TASK_REPLAN_LIMIT:
        log("🚫", f"{task['id']} 已达到最大重规划次数 ({TASK_REPLAN_LIMIT})，跳过重规划", "red")
        return False

    log("🔬", f"Codex 诊断任务失败原因并重规划 ...", "cyan")
    repo_context = get_repo_context()
    env_guidance = get_execution_environment_guidance()

    prompt = f"""
你是 Windows 桌面软件工程师，正在诊断一个 C++/Win32 任务的失败原因。

{WIN32_PROJECT_GUIDANCE}

当前仓库上下文：
{repo_context}

{env_guidance}

失败任务：
- ID：{task['id']}
- 描述：{task['desc']}
- 错误信息：{trim_text(error, 3000)}

请判断：
1. 这个任务是否需要拆分成更小的前置任务？
2. 还是任务定义本身需要调整（修改 desc、files_hint、test_commands）？

请用以下 JSON 格式输出（只输出 JSON，不要其他文字）：
{{
  "action": "replan" 或 "update",
  "reason": "原因说明",
  "new_tasks": [  // action=replan 时填入前置任务列表，action=update 时为空数组
    {{
      "id": "auto",
      "phase": "foundation",
      "desc": "...",
      "depends_on": [],
      "files_hint": [],
      "test_hint": "...",
      "test_commands": []
    }}
  ],
  "updated_task": {{  // action=update 时填入更新后的任务定义，action=replan 时可为 null
    "desc": "...",
    "files_hint": [],
    "test_hint": "...",
    "test_commands": []
  }}
}}
"""
    raw = call_codex(prompt, label="Codex-Replan", output_schema=CODEX_REPLAN_SCHEMA)
    try:
        plan = extract_json_payload(raw, dict)
    except ValueError:
        log("⚠️", "重规划输出解析失败，跳过重规划", "yellow")
        return False

    action = plan.get("action", "").strip()
    current_index = next((i for i, t in enumerate(tasks) if t["id"] == task["id"]), None)
    if current_index is None:
        return False

    task["replans"] = task.get("replans", 0) + 1
    new_tasks = []
    error = plan.get("reason", "")

    if action == "replan":
        raw_new = plan.get("new_tasks") or []
        if raw_new:
            new_tasks = normalize_new_tasks(raw_new, tasks)
            for nt in new_tasks:
                nt["depends_on"] = [t["id"] for t in tasks[:current_index] if t.get("status") == "done"]
            task["depends_on"] = [nt["id"] for nt in new_tasks]
    elif action == "update":
        updated = plan.get("updated_task") or {}
        if updated.get("desc"):
            task["desc"] = updated["desc"]
        if updated.get("files_hint"):
            task["files_hint"] = _normalize_str_list(updated["files_hint"])
        if updated.get("test_hint"):
            task["test_hint"] = updated["test_hint"]
        if updated.get("test_commands") is not None:
            task["test_commands"] = _normalize_str_list(updated["test_commands"])
        task["status"] = "todo"
        task["retries"] = 0
    else:
        log("⚠️", f"未知重规划 action: {action}，跳过", "yellow")
        return False

    task["last_error"] = trim_text(error, 5000)
    tasks[current_index] = task
    if new_tasks:
        tasks[current_index:current_index] = new_tasks

    save_tasks(tasks)
    if new_tasks:
        log("🔀", f"{task['id']} 已自动重规划，并插入 {len(new_tasks)} 个前置任务", "yellow")
    else:
        log("🔁", f"{task['id']} 已自动更新任务定义，将按新策略继续执行", "yellow")
    return True


# =========================
# 工具：重置 tasks.json 重试计数
# =========================
def reset_retries():
    if not Path(TASK_FILE).exists():
        log("❌", f"找不到 {TASK_FILE}，请先生成任务", "red")
        sys.exit(1)
    with open(TASK_FILE, encoding="utf-8") as fh:
        tasks = json.load(fh)
    reset_count = 0
    revived_count = 0
    for task in tasks:
        if task.get("retries", 0) != 0:
            task["retries"] = 0
            reset_count += 1
        if task.get("status") in {"failed", "blocked"}:
            task["status"] = "todo"
            revived_count += 1
        task["blocked_by"] = []
        task["last_error"] = ""
        task["diagnosis"] = ""
    save_tasks(tasks)
    log("✅", f"已重置 {reset_count} 个任务的 retries → 0", "green")
    if revived_count:
        log("🔄", f"已将 {revived_count} 个 failed 任务恢复为 todo", "cyan")
    log("💡", "现在可以用 --tasks 重新执行所有未完成的任务", "yellow")


# =========================
# 主循环（Orchestrator）
# =========================
def check_prerequisites():
    """检查必要的工具是否可用。"""
    missing = []
    if not shutil.which("codex"):
        missing.append("codex CLI（npm install -g @openai/codex）")
    if not shutil.which("git"):
        missing.append("git")

    # Windows 构建工具（可选，有则提示）
    has_cmake = bool(shutil.which("cmake"))
    has_msbuild = bool(shutil.which("msbuild"))
    if not has_cmake:
        log("⚠️", "未找到 cmake，C++ 构建验证将无法自动运行（需手动安装 CMake）", "yellow")
    if not has_msbuild:
        log("⚠️", "未找到 msbuild，C++ 构建验证将无法自动运行（需安装 Visual Studio 或 Build Tools）", "yellow")
    if has_cmake and os.name == "nt":
        vcvars_path = get_preferred_vcvars_path()
        if vcvars_path:
            log("🧰", f"已检测到 vcvars64: {vcvars_path}", "cyan")
        else:
            log("⚠️", "未检测到可用的 vcvars64.bat，MSVC/NMake 验证可能不稳定", "yellow")

        generator_flags = _pick_cmake_generator_flags()
        if generator_flags:
            log("🧪", f"CMake 自动验证将使用 {generator_flags.strip()}", "cyan")
        elif vcvars_path:
            log("🧪", "未探测到稳定的显式生成器，将依赖 vcvars 环境下的默认 CMake generator", "yellow")

    if missing:
        for item in missing:
            log("❌", f"缺少必要工具：{item}", "red")
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description="Win32 桌面图标移动工具 AI Agent")
    parser.add_argument("--req",           type=str,          help="直接传入需求文本")
    parser.add_argument("--prd",           action="store_true", help="跳过需求→PRD，直接读 prd.txt")
    parser.add_argument("--tasks",         action="store_true", help="跳过PRD→拆任务，直接读 tasks.json")
    parser.add_argument("--reset-retries", action="store_true", help="重置重试计数，恢复失败任务后退出")
    args = parser.parse_args()

    if args.reset_retries:
        print(c("\n🔧 重置任务重试计数\n", "bold"))
        reset_retries()
        sys.exit(0)

    print(c("\n🚀 Win32 桌面图标移动工具 AI Agent 启动\n", "bold"))
    check_prerequisites()

    # ── Step 1: 获取需求 & 生成 PRD ──────────────────────────
    if args.tasks:
        if not Path(TASK_FILE).exists():
            log("❌", f"找不到 {TASK_FILE}", "red")
            sys.exit(1)
        with open(TASK_FILE, encoding="utf-8") as fh:
            tasks = stable_topological_sort(normalize_tasks(json.load(fh)))
        log("📂", f"直接加载 {len(tasks)} 个任务", "cyan")

    else:
        if args.prd:
            if not Path(PRD_FILE).exists():
                log("❌", f"找不到 {PRD_FILE}", "red")
                sys.exit(1)
            prd = Path(PRD_FILE).read_text(encoding="utf-8")
            log("📂", f"直接读取 {PRD_FILE}", "cyan")
        else:
            if args.req:
                requirement = args.req
            else:
                print(c("请输入你的需求（输入完成后单独一行输入 END 回车结束）：", "cyan"))
                lines = []
                try:
                    while True:
                        line = input()
                        if line.strip().upper() == "END":
                            break
                        lines.append(line)
                except (KeyboardInterrupt, EOFError):
                    pass
                requirement = "\n".join(lines).strip()
            if not requirement:
                log("❌", "需求不能为空", "red")
                sys.exit(1)
            prd = requirement_to_prd(requirement)

        # ── Step 2: PRD → Tasks ──────────────────────────────
        tasks = prd_to_tasks(prd)

    # ── Step 3~5: 逐任务执行 ─────────────────────────────────
    total = len(tasks)
    skipped = sum(1 for task in tasks if task.get("status") == "done")
    terminal_failed_ids: set[str] = set()

    while True:
        processed_task = False
        state_changed = False

        for idx, task in enumerate(tasks, 1):
            if task.get("status") == "done":
                continue
            if task["id"] in terminal_failed_ids:
                continue

            blockers = unresolved_dependencies(task, tasks, terminal_failed_ids)
            if blockers:
                if task.get("status") != "blocked" or task.get("blocked_by") != blockers:
                    task["status"] = "blocked"
                    task["blocked_by"] = blockers
                    save_tasks(tasks)
                    state_changed = True
                continue

            if task.get("status") == "blocked":
                task["status"] = "todo"
                task["blocked_by"] = []
                save_tasks(tasks)
                state_changed = True

            total = len(tasks)
            print(c(f"\n{'─'*60}", "cyan"))
            log("🚀", f"[{idx}/{total}] 开始任务: {task['desc']}", "bold")

            success = False
            attempt = 0
            error = task.get("last_error") or None
            restore_point = create_restore_point(task["id"])

            try:
                for attempt in range(1, MAX_RETRY + 1):
                    log("🔧", f"第 {attempt} 次尝试 ...", "cyan")

                    execute_task(task, tasks=tasks, error=error)

                    test_ok, test_output = auto_test(task)
                    if not test_ok:
                        log("❌", "构建验证失败，准备重试", "red")
                        print(trim_text(test_output, 800))
                        error = f"构建验证失败：\n{trim_text(test_output, 5000)}"
                        task["last_error"] = trim_text(error, 5000)
                        git_reset(restore_point)
                        continue

                    diff = git_diff_since(restore_point)
                    review_passed, review_text = codex_review(task, diff, validation_summary=test_output)

                    print(c("\n── Codex 审核结果 ──", "cyan"))
                    print(trim_text(review_text, 800))

                    if not review_passed:
                        log("❌", "Codex 审核不通过，准备重试", "red")
                        error = f"Codex 代码审核不通过，修复建议：\n{trim_text(review_text, 5000)}"
                        task["last_error"] = trim_text(error, 5000)
                        git_reset(restore_point)
                        continue

                    success = True
                    task["status"] = "done"
                    task["retries"] = attempt
                    task["review_result"] = "PASS"
                    task["blocked_by"] = []
                    task["last_error"] = ""
                    save_tasks(tasks)
                    git_commit_task(task, restore_point)
                    log("✅", f"任务完成: {task['desc']}", "green")
                    break
            finally:
                cleanup_restore_point(restore_point)

            if success:
                processed_task = True
                break

            task["status"] = "failed"
            task["retries"] = attempt
            task["last_error"] = trim_text(error or "任务执行失败", 5000)
            save_tasks(tasks)

            if diagnose_and_replan_task(task, tasks, task["last_error"]):
                processed_task = True
                break

            terminal_failed_ids.add(task["id"])
            log("💥", f"任务失败（重试{MAX_RETRY}次）: {task['desc']}", "red")
            processed_task = True
            break

        if processed_task:
            continue
        if not state_changed:
            break

    # ── 汇总报告 ──────────────────────────────────────────────
    total = len(tasks)
    done_count    = sum(1 for t in tasks if t["status"] == "done")
    failed_count  = sum(1 for t in tasks if t["status"] == "failed")
    blocked_count = sum(1 for t in tasks if t["status"] == "blocked")

    print(f"\n{c('═'*64, 'bold')}")
    print(f"{c(' 📊 任务执行汇总报告', 'bold')}")
    print(f"{c('─'*64, 'cyan')}")

    for idx, t in enumerate(tasks, 1):
        status_icon = (
            c("✅ 完成", "green")  if t["status"] == "done"    else
            c("❌ 失败", "red")    if t["status"] == "failed"  else
            c("⛔ 阻塞", "yellow") if t["status"] == "blocked" else
            c("⏭️ 跳过", "yellow")
        )
        retry_info = f" (重试 {t['retries']} 次)" if t.get("retries", 0) > 0 else ""
        print(f"  {idx:02d}. [{t['id']}] {t['desc'][:45]:<45} {status_icon}{retry_info}")

    print(f"{c('─'*64, 'cyan')}")
    summary_line = (
        f"   共计: {total} | "
        f"{c('完成: ' + str(done_count), 'green')} | "
        f"{c('失败: ' + str(failed_count), 'red')} | "
        f"{c('阻塞: ' + str(blocked_count), 'yellow')} | "
        f"{c('跳过: ' + str(skipped), 'yellow')}"
    )
    print(summary_line)

    if Path(REVIEW_DIR).exists():
        print(f"\n   📂 审核报告已保存在: {c(REVIEW_DIR + '/', 'cyan')}")

    if failed_count > 0 or blocked_count > 0:
        print(f"   ⚠️  当前仍有 {failed_count} 个失败任务、{blocked_count} 个阻塞任务。")
        print(f"   💡  可使用 --reset-retries 后再次运行 --tasks 继续推进。")
    else:
        print(f"   ✨ 所有任务已顺利完成！")

    print(f"{c('═'*64, 'bold')}\n")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print()
        log("⏹️", "已收到中断信号，当前任务已安全停止。", "yellow")
        sys.exit(130)
