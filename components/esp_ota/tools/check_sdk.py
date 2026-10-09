#!/usr/bin/env python3
"""Verify the exact public IDF/lwIP source combination used by esp-ota."""

import argparse
import json
import os
import subprocess
from pathlib import Path


LOCK = json.loads((Path(__file__).resolve().parent.parent / "sdk-lock.json").read_text())


def git_environment() -> dict:
    for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR", "GIT_INDEX_FILE",
                 "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES",
                 "GIT_REPLACE_REF_BASE", "GIT_GRAFT_FILE"):
        if os.environ.get(name):
            raise ValueError(f"Git 环境不能重定向来源或 alternate 对象：{name}")
    # Every Git command reads the actual locked objects, regardless of caller flags.
    return {**os.environ, "GIT_NO_REPLACE_OBJECTS": "1"}


def git(path: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(path), *args],
        check=True,
        capture_output=True,
        text=True,
        env=git_environment(),
    )
    return result.stdout.rstrip("\n")


def verify_complete_repository(path: Path, source_root: Path | None = None) -> None:
    git_environment()
    def git_path(*arguments: str) -> Path:
        value = Path(git(path, *arguments))
        return value if value.is_absolute() else path / value

    git_metadata = path / ".git"
    git_directory_path = git_path("rev-parse", "--git-dir")
    if git_metadata.is_symlink() or git_directory_path.is_symlink():
        raise ValueError(f"SDK Git 元数据不能以符号链接借用其他来源：{path}")
    git_directory = git_directory_path.resolve(strict=True)
    common_directory = git_path("rev-parse", "--git-common-dir").resolve(strict=True)
    if git_directory != common_directory:
        raise ValueError(f"SDK 来源不能使用借用主仓对象库的 linked worktree：{path}")
    source_root = (source_root or path).resolve(strict=True)
    if not path.resolve().is_relative_to(source_root):
        raise ValueError(f"SDK 子来源必须位于完整根来源目录：{path}")
    if path.resolve() == source_root:
        if not git_directory.is_relative_to(source_root):
            raise ValueError(f"SDK 根 Git 元数据必须位于来源自身目录：{path}")
    elif git_metadata.is_file():
        root_git_directory = Path(git(source_root, "rev-parse", "--absolute-git-dir")).resolve(strict=True)
        if not git_directory.is_relative_to(root_git_directory / "modules"):
            raise ValueError(f"SDK absorbed 子模块 Git 元数据必须归属根来源的 modules：{path}")
    elif git_directory != (path / ".git").resolve(strict=True):
        raise ValueError(f"SDK 子来源必须拥有自身 .git 目录：{path}")
    if git(path, "for-each-ref", "--format=%(refname)", "refs/replace/"):
        raise ValueError(f"SDK 来源不能包含 replace 对象引用：{path}")
    grafts = git_path("rev-parse", "--git-path", "info/grafts")
    if grafts.exists() or grafts.is_symlink():
        raise ValueError(f"SDK 来源不能包含 grafts 历史替换：{path}")
    if git_metadata.is_file():
        binding = subprocess.run(
            ["git", "-C", str(path), "config", "--local", "--path", "--get", "core.worktree"],
            text=True, capture_output=True, env=git_environment())
        if binding.returncode or not binding.stdout.strip():
            raise ValueError(f"SDK Git 元数据文件必须原生绑定当前来源：{path}")
        worktree = Path(binding.stdout.rstrip("\n"))
        if not worktree.is_absolute():
            worktree = git_directory / worktree
        if worktree.resolve() != path.resolve():
            raise ValueError(f"SDK Git 元数据文件指向另一工作树：{path}")
    objects = git_path("rev-parse", "--git-path", "objects")
    if (objects.is_symlink() or not objects.is_dir()
            or objects.resolve() != git_directory / "objects"
            or any(item.is_symlink() for item in objects.rglob("*"))):
        raise ValueError(f"SDK 来源对象库必须归属于该独立仓库，不能以符号链接借用对象：{path}")
    alternate = Path(git(path, "rev-parse", "--git-path", "objects/info/alternates"))
    if not alternate.is_absolute():
        alternate = path / alternate
    if alternate.exists() or alternate.is_symlink():
        raise ValueError(f"SDK 来源不能通过 alternates 借用其他仓库对象：{path}")
    if git(path, "rev-parse", "--show-toplevel") != str(path.resolve()):
        raise ValueError(f"SDK 来源未独立初始化：{path}")
    if git(path, "rev-parse", "--is-shallow-repository") != "false":
        raise ValueError(f"SDK 来源必须保有完整历史，不能使用 shallow clone：{path}")
    for line in git(path, "config", "--list").splitlines():
        key, _, value = line.partition("=")
        if key == "extensions.partialclone" or (
                key.startswith("remote.") and key.endswith((".promisor", ".partialclonefilter"))):
            raise ValueError(f"SDK 来源不能使用 partial clone：{path}")
        if key in ("core.sparsecheckout", "core.sparsecheckoutcone") and value.lower() in (
                "true", "yes", "on", "1"):
            raise ValueError(f"SDK 来源不能使用 sparse checkout：{path}")
    result = subprocess.run(["git", "-C", str(path), "fsck", "--connectivity-only",
                             "--no-dangling"], text=True, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, env=git_environment())
    if result.returncode:
        raise ValueError(f"SDK 来源对象不完整：{path}\n{result.stderr.strip()}")


def check(path: Path) -> None:
    idf = path.resolve()
    lwip = idf / LOCK["lwip"]["path"]
    if git(idf, "rev-parse", "HEAD") != LOCK["idf"]["revision"]:
        raise ValueError("ESP-IDF commit differs from sdk-lock.json")
    if not lwip.is_dir() or git(lwip, "rev-parse", "HEAD") != LOCK["lwip"]["revision"]:
        raise ValueError("ESP lwIP commit differs from sdk-lock.json")
    if git(lwip, "status", "--porcelain", "--untracked-files=normal",
           "--ignore-submodules=none"):
        raise ValueError("ESP lwIP worktree has tracked changes")
    status = git(idf, "status", "--porcelain", "--untracked-files=normal",
                 "--ignore-submodules=none")
    if status.splitlines() != [f" M {LOCK['lwip']['path']}"]:
        raise ValueError("ESP-IDF worktree differs beyond the locked lwIP gitlink")
    verify_complete_repository(idf)
    for line in git(idf, "submodule", "status", "--recursive").splitlines():
        parts = line.strip().split()
        if len(parts) < 2:
            raise ValueError("ESP-IDF submodule status is invalid")
        revision, submodule_path = parts[:2]
        if revision.startswith(("-", "U")):
            raise ValueError(f"ESP-IDF submodule is unavailable: {submodule_path}")
        if revision.startswith("+") and (submodule_path != LOCK["lwip"]["path"]
                                         or revision[1:] != LOCK["lwip"]["revision"]):
            raise ValueError(f"ESP-IDF submodule differs from its gitlink: {submodule_path}")
        source = idf / submodule_path
        if git(source, "status", "--porcelain", "--untracked-files=normal",
               "--ignore-submodules=none"):
            raise ValueError(f"SDK 递归源码存在未提交内容：{source}")
        verify_complete_repository(source, source_root=idf)



def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--path", required=True, type=Path)
    args = parser.parse_args()
    try:
        check(args.path)
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        parser.exit(1, f"esp-ota SDK check failed: {error}\n")
    print("esp-ota SDK source matches sdk-lock.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
