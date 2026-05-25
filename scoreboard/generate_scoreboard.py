#!/usr/bin/env python3
"""
从已挂载的大赛磁盘生成本地 scoreboard。

生成内容只描述“磁盘里有哪些测例、项目当前默认跑哪些测例、每个小测例在哪里”，
不负责真实执行测例。这样可以把评测入口、磁盘脚本和调试导航分开维护。
"""

from __future__ import annotations

import argparse
import shutil
import shlex
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


ARCH_MOUNT_DEFAULTS = {
    "riscv": Path("/mnt/sdcard-rv"),
    "loongarch": Path("/mnt/sdcard-la"),
}

LIBC_NAMES = ("musl", "glibc")

# 和 user/user_lib/user_test.cc 中 basic_testcases 保持一致。
PROJECT_BASIC_CASES = [
    "write",
    "fork",
    "exit",
    "wait",
    "getpid",
    "getppid",
    "dup",
    "dup2",
    "execve",
    "getcwd",
    "gettimeofday",
    "yield",
    "sleep",
    "times",
    "clone",
    "brk",
    "waitpid",
    "mmap",
    "fstat",
    "uname",
    "openat",
    "open",
    "close",
    "read",
    "getdents",
    "mkdir_",
    "chdir",
    "mount",
    "umount",
    "munmap",
    "unlink",
    "pipe",
]

# 当前 initcode 默认只开启 musl/basic 和 musl/libctest。
PROJECT_DEFAULT_GROUPS = {
    ("musl", "basic"),
    ("musl", "libctest-static"),
    ("musl", "libctest-dynamic"),
}


@dataclass
class ScoreCase:
    arch: str
    libc: str
    group: str
    name: str
    command: str
    path: str
    url: str
    source: str
    project_default: bool
    status: str = ""
    note: str = ""


def path_url(path: Path) -> str:
    return path.resolve().as_uri() if path.exists() else ""


def read_lines(path: Path) -> list[str]:
    if not path.exists():
        return []
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def shell_words(line: str) -> list[str]:
    try:
        return shlex.split(line, comments=False, posix=True)
    except ValueError:
        return line.split()


def is_command_line(line: str) -> bool:
    stripped = line.strip()
    if not stripped or stripped.startswith("#"):
        return False
    if stripped in {"do", "done", "then", "else", "fi"}:
        return False
    if stripped.endswith("{") or stripped.startswith(("if ", "for ", "while ", "case ")):
        return False
    if "=" in stripped and not stripped.startswith(("./", "$", "busybox", "cp ", "kill ", "sleep ")):
        return False
    return True


def script_commands(script: Path, prefixes: Iterable[str] | None = None) -> list[tuple[str, str]]:
    result: list[tuple[str, str]] = []
    allowed = tuple(prefixes or ())
    for line in read_lines(script):
        stripped = line.strip()
        if not is_command_line(stripped):
            continue
        if allowed and not stripped.startswith(allowed):
            continue
        words = shell_words(stripped)
        name = words[0].removeprefix("./") if words else stripped[:40]
        result.append((name, stripped))
    return result


def parse_libctest_script(script: Path, entry_name: str) -> list[tuple[str, str]]:
    cases: list[tuple[str, str]] = []
    for line in read_lines(script):
        stripped = line.strip()
        if "runtest.exe" not in stripped or entry_name not in stripped:
            continue
        words = shell_words(stripped)
        if len(words) >= 4:
            cases.append((words[-1], stripped))
    return cases


def add_case(
    cases: list[ScoreCase],
    arch: str,
    libc: str,
    group: str,
    name: str,
    command: str,
    path: Path,
    source: str,
    project_default: bool | None = None,
    note: str = "",
    status: str = "",
) -> None:
    if project_default is None:
        project_default = (libc, group) in PROJECT_DEFAULT_GROUPS
        if group == "basic":
            project_default = project_default and name in PROJECT_BASIC_CASES
    cases.append(
        ScoreCase(
            arch=arch,
            libc=libc,
            group=group,
            name=name,
            command=command,
            path=str(path),
            url=path_url(path),
            source=source,
            project_default=project_default,
            status=status,
            note=note,
        )
    )


def scan_basic(cases: list[ScoreCase], arch: str, libc: str, libc_root: Path) -> None:
    basic_dir = libc_root / "basic"
    if not basic_dir.exists():
        return
    data_files = {"run-all.sh", "text.txt", "test_close.txt", "test_mmap.txt", "test_echo"}
    for item in sorted(p for p in basic_dir.iterdir() if p.is_file()):
        if item.name in data_files:
            continue
        add_case(cases, arch, libc, "basic", item.name, f"./basic/{item.name}", item, "disk-basic")


def scan_lua(cases: list[ScoreCase], arch: str, libc: str, libc_root: Path) -> None:
    script = libc_root / "lua_testcode.sh"
    for line in read_lines(script):
        words = shell_words(line.strip())
        if len(words) >= 2 and words[0].endswith("test.sh") and words[1].endswith(".lua"):
            lua_file = libc_root / words[1]
            add_case(cases, arch, libc, "lua", words[1], line.strip(), lua_file, "disk-script")


def scan_busybox(cases: list[ScoreCase], arch: str, libc: str, libc_root: Path) -> None:
    cmd_file = libc_root / "busybox_cmd.txt"
    for index, line in enumerate(read_lines(cmd_file), start=1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        add_case(
            cases,
            arch,
            libc,
            "busybox",
            stripped,
            f"./busybox {stripped}",
            cmd_file,
            "disk-command-list",
            project_default=False,
            note=f"busybox_cmd.txt:{index}",
        )


def scan_libctest(cases: list[ScoreCase], arch: str, libc: str, libc_root: Path) -> None:
    for group, script_name, entry_name in (
        ("libctest-static", "run-static.sh", "entry-static.exe"),
        ("libctest-dynamic", "run-dynamic.sh", "entry-dynamic.exe"),
    ):
        script = libc_root / script_name
        for name, command in parse_libctest_script(script, entry_name):
            add_case(cases, arch, libc, group, name, command, script, "disk-libctest-script")


def scan_ltp(cases: list[ScoreCase], arch: str, libc: str, libc_root: Path) -> None:
    ltp_bin = libc_root / "ltp" / "testcases" / "bin"
    if not ltp_bin.exists():
        return
    for item in sorted(p for p in ltp_bin.iterdir() if p.is_file()):
        add_case(
            cases,
            arch,
            libc,
            "ltp",
            item.name,
            item.name,
            item,
            "disk-ltp-bin",
            project_default=False,
        )


def scan_script_groups(cases: list[ScoreCase], arch: str, libc: str, libc_root: Path) -> None:
    script_specs = {
        "iozone": ("iozone_testcode.sh", ("./iozone",)),
        "libcbench": ("libcbench_testcode.sh", ("./libc-bench",)),
        "lmbench": ("lmbench_testcode.sh", ("./lmbench_all",)),
        "cyclictest": ("cyclictest_testcode.sh", ("run_cyclictest", "./cyclictest")),
        "iperf": ("iperf_testcode.sh", ("run_iperf", "$iperf")),
        "netperf": ("netperf_testcode.sh", ("run_netperf", "./netperf")),
        "unixbench": ("unixbench_testcode.sh", ("./", "UB_BINDIR=./")),
    }
    for group, (script_name, prefixes) in script_specs.items():
        script = libc_root / script_name
        for index, (name, command) in enumerate(script_commands(script, prefixes), start=1):
            add_case(
                cases,
                arch,
                libc,
                group,
                f"{index:02d}-{name}",
                command,
                script,
                "disk-testcode-script",
                project_default=False,
            )


def scan_mounts(arch_roots: dict[str, Path]) -> tuple[list[ScoreCase], dict[str, object]]:
    cases: list[ScoreCase] = []
    mounts: dict[str, object] = {}
    for arch, root in arch_roots.items():
        mounts[arch] = {"root": str(root), "exists": root.exists()}
        if not root.exists():
            continue
        for libc in LIBC_NAMES:
            libc_root = root / libc
            mounts[f"{arch}/{libc}"] = {"root": str(libc_root), "exists": libc_root.exists()}
            if not libc_root.exists():
                continue
            scan_basic(cases, arch, libc, libc_root)
            scan_lua(cases, arch, libc, libc_root)
            scan_busybox(cases, arch, libc, libc_root)
            scan_libctest(cases, arch, libc, libc_root)
            scan_ltp(cases, arch, libc, libc_root)
            scan_script_groups(cases, arch, libc, libc_root)
    return cases, mounts


def summarize(cases: list[ScoreCase], mounts: dict[str, object]) -> dict[str, object]:
    by_key: dict[str, int] = {}
    for case in cases:
        key = f"{case.arch}/{case.libc}/{case.group}"
        by_key[key] = by_key.get(key, 0) + 1
    return {
        "total_cases": len(cases),
        "groups": dict(sorted(by_key.items())),
        "mounts": mounts,
    }


def md_escape(value: object) -> str:
    text = str(value)
    return (
        text.replace("\\", "\\\\")
        .replace("|", "\\|")
        .replace("\n", "<br>")
        .replace("\r", "")
    )


def md_link(label: str, target: str) -> str:
    if not target:
        return md_escape(label)
    return f"[{md_escape(label)}]({target})"


def relative_link(from_file: Path, target: Path) -> str:
    try:
        return target.resolve().relative_to(from_file.parent.resolve()).as_posix()
    except ValueError:
        return target.resolve().as_posix()


def markdown_link_label(cell: str) -> str:
    cell = cell.strip()
    if cell.startswith("[") and "](" in cell:
        return cell[1 : cell.index("](")]
    return cell


def read_existing_case_notes(out_dir: Path) -> dict[tuple[str, str, str, str], tuple[str, str]]:
    case_notes: dict[tuple[str, str, str, str], tuple[str, str]] = {}
    for arch in ARCH_MOUNT_DEFAULTS:
        for libc in LIBC_NAMES:
            libc_dir = out_dir / arch / libc
            if not libc_dir.exists():
                continue
            for page in libc_dir.glob("*.md"):
                if page.name == "README.md":
                    continue
                group = page.stem
                lines = read_lines(page)
                header: list[str] = []
                for line in lines:
                    if line.startswith("| 测例 |"):
                        header = [part.strip() for part in line.strip().strip("|").split("|")]
                        continue
                    if not header or not line.startswith("|"):
                        continue
                    parts = [part.strip() for part in line.strip().strip("|").split("|")]
                    if len(parts) < len(header) or parts[0] == "---":
                        continue
                    name = markdown_link_label(parts[0])
                    status_index = 1
                    note_index = header.index("备注") if "备注" in header else -1
                    status = parts[status_index]
                    note = parts[note_index] if note_index >= 0 else ""
                    if status or note:
                        case_notes[(arch, libc, group, name)] = (status, note)
    return case_notes


def apply_existing_case_notes(cases: list[ScoreCase], case_notes: dict[tuple[str, str, str, str], tuple[str, str]]) -> None:
    for case in cases:
        status, note = case_notes.get((case.arch, case.libc, case.group, case.name), (case.status, case.note))
        case.status = status
        case.note = note


def is_pass_status(status: str) -> bool:
    return status.strip().upper() == "PASS"


def render_case_table(cases: list[ScoreCase]) -> str:
    lines = [
        "| 测例 | 是否通过 | 备注 |",
        "| --- | --- | --- |",
    ]
    for case in cases:
        lines.append(
            "| "
            + " | ".join(
                [
                    md_escape(case.name),
                    md_escape(case.status),
                    md_escape(case.note),
                ]
            )
            + " |"
        )
    return "\n".join(lines) + "\n"


def render_group_summary_table(
    out_dir: Path,
    readme_path: Path,
    cases_by_group: dict[tuple[str, str, str], list[ScoreCase]],
    arch: str,
    libc: str,
) -> str:
    lines = [
        "| 小分 | 总测例 | Pass测例 | 记录文件 |",
        "| --- | ---: | ---: | --- |",
    ]
    found = False
    total_count = 0
    total_pass = 0
    for (case_arch, case_libc, group), group_cases in sorted(cases_by_group.items()):
        if case_arch != arch or case_libc != libc:
            continue
        found = True
        group_page = out_dir / arch / libc / f"{group}.md"
        pass_count = sum(1 for case in group_cases if is_pass_status(case.status))
        total_count += len(group_cases)
        total_pass += pass_count
        group_link = md_link(group, relative_link(readme_path, group_page))
        file_link = md_link(group_page.name, relative_link(readme_path, group_page))
        lines.append(f"| {group_link} | {len(group_cases)} | {pass_count} | {file_link} |")
    if not found:
        lines.append("| 当前未挂载或未抽取到测例 | 0 | 0 |  |")
    else:
        lines.append(f"| **合计** | **{total_count}** | **{total_pass}** |  |")
    return "\n".join(lines)


def write_outputs(out_dir: Path, cases: list[ScoreCase], summary: dict[str, object]) -> None:
    stale_html_dir = out_dir / "out"
    if stale_html_dir.exists():
        # 只清理旧版 HTML/JSON 生成目录，避免 Markdown scoreboard 和脚本混在一起。
        shutil.rmtree(stale_html_dir)

    out_dir.mkdir(parents=True, exist_ok=True)
    apply_existing_case_notes(cases, read_existing_case_notes(out_dir))

    cases_by_group: dict[tuple[str, str, str], list[ScoreCase]] = {}
    for case in cases:
        cases_by_group.setdefault((case.arch, case.libc, case.group), []).append(case)

    group_pages: dict[tuple[str, str, str], Path] = {}
    for (arch, libc, group), group_cases in sorted(cases_by_group.items()):
        page = out_dir / arch / libc / f"{group}.md"
        page.parent.mkdir(parents=True, exist_ok=True)
        group_pages[(arch, libc, group)] = page
        page.write_text(
            "\n".join(
                [
                    f"# {arch}/{libc}/{group}",
                    "",
                    f"测例数量：{len(group_cases)}",
                    "",
                    render_case_table(group_cases),
                ]
            ),
            encoding="utf-8",
        )

    for arch in ARCH_MOUNT_DEFAULTS:
        for libc in LIBC_NAMES:
            libc_dir = out_dir / arch / libc
            libc_dir.mkdir(parents=True, exist_ok=True)
            (libc_dir / "README.md").write_text(
                "\n".join(
                    [
                        f"# {arch}/{libc} Scoreboard",
                        "",
                        "## 小分",
                        "",
                        render_group_summary_table(out_dir, libc_dir / "README.md", cases_by_group, arch, libc),
                        "",
                    ]
                ),
                encoding="utf-8",
            )

    root_lines = [
        "# F7LY Test Scoreboard",
        "",
        "这个目录是从已挂载的大赛磁盘抽取出来的 Markdown scoreboard。",
        "它只记录测例结构和协作状态，不负责执行测例。",
        "",
        "顶层直接汇总四个组合；每个小分链接到对应 Markdown 文件。",
        "Pass测例按小分文件中“是否通过”列的 `PASS` 数统计。没有真实运行结果时不会自动标 PASS。",
        "",
    ]
    for arch in ARCH_MOUNT_DEFAULTS:
        for libc in LIBC_NAMES:
            root_lines.extend(
                [
                    f"## {arch} {libc}",
                    "",
                    render_group_summary_table(out_dir, out_dir / "README.md", cases_by_group, arch, libc),
                    "",
                ]
            )

    root_lines.extend(["", "## 挂载状态", "", "| 目标 | 路径 | 状态 |", "| --- | --- | --- |"])
    for key, value in summary["mounts"].items():
        assert isinstance(value, dict)
        root_lines.append(
            f"| `{md_escape(key)}` | `{md_escape(value['root'])}` | {'存在' if value['exists'] else '缺失'} |"
        )

    root_lines.extend(
        [
            "",
            "## 当前项目默认回归",
            "",
            "当前 `regression_suite_4d1444()` 默认开启 `musl/basic` 和 `musl/libctest`。",
            "小分文件只记录协作状态；测例命令和来源由生成器从挂载磁盘重新扫描。",
            "",
        ]
    )
    (out_dir / "README.md").write_text("\n".join(root_lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="生成 F7LY 本地测试 scoreboard")
    parser.add_argument("--riscv-root", type=Path, default=ARCH_MOUNT_DEFAULTS["riscv"])
    parser.add_argument("--loongarch-root", type=Path, default=ARCH_MOUNT_DEFAULTS["loongarch"])
    parser.add_argument("--out", type=Path, default=Path("scoreboard"))
    args = parser.parse_args()

    cases, mounts = scan_mounts(
        {
            "riscv": args.riscv_root,
            "loongarch": args.loongarch_root,
        }
    )
    summary = summarize(cases, mounts)
    write_outputs(args.out, cases, summary)
    print(f"生成完成: {args.out / 'README.md'}")
    print(f"测例总数: {summary['total_cases']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
