"""用真实 Git 对象库验证 SDK 来源不得借用外部对象。"""
import importlib.util
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("check_sdk", Path(__file__).resolve().parents[1] / "components/esp_ota/tools/check_sdk.py")
SDK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SDK)


class SDKObjectOwnershipTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.source = self.root / "source"
        self.source.mkdir()
        self.git(self.source, "init", "-q", "-b", "master")
        self.git(self.source, "config", "user.name", "SDK fixture")
        self.git(self.source, "config", "user.email", "sdk@example.invalid")
        (self.source / "source.c").write_text("int source;\n")
        self.git(self.source, "add", ".")
        self.git(self.source, "commit", "-qm", "fixture")

    @staticmethod
    def git(path, *args):
        return subprocess.run(["git", "-C", str(path), *args], check=True,
                              capture_output=True, text=True).stdout.strip()

    def test_accepts_complete_self_owned_source(self):
        SDK.verify_complete_repository(self.source)

    def test_rejects_shared_clone_even_when_fsck_passes(self):
        shared = self.root / "shared"
        self.git(self.root, "clone", "-q", "--shared", str(self.source), str(shared))
        self.git(shared, "fsck", "--connectivity-only", "--no-dangling")
        with self.assertRaisesRegex(ValueError, "alternates"):
            SDK.verify_complete_repository(shared)

    def test_rejects_symlinked_git_directory_even_when_fsck_passes(self):
        alias = self.root / "symlinked-git-directory"
        shutil.copytree(self.source, alias, ignore=shutil.ignore_patterns(".git"))
        (alias / ".git").symlink_to(self.source / ".git", target_is_directory=True)
        self.git(alias, "fsck", "--connectivity-only", "--no-dangling")
        with self.assertRaisesRegex(ValueError, "Git 元数据"):
            SDK.verify_complete_repository(alias)

    def test_rejects_unbound_gitfile_even_when_fsck_passes(self):
        alias = self.root / "unbound-gitfile"
        shutil.copytree(self.source, alias, ignore=shutil.ignore_patterns(".git"))
        (alias / ".git").write_text("gitdir: " + str(self.source / ".git") + "\n")
        self.git(alias, "fsck", "--connectivity-only", "--no-dangling")
        with self.assertRaisesRegex(ValueError, "Git 元数据"):
            SDK.verify_complete_repository(alias)

    def test_rejects_linked_worktree_even_when_fsck_passes(self):
        linked = self.root / "linked"
        self.git(self.source, "worktree", "add", "-q", "--detach", str(linked), "HEAD")
        try:
            self.git(linked, "fsck", "--connectivity-only", "--no-dangling")
            with self.assertRaisesRegex(ValueError, "linked"):
                SDK.verify_complete_repository(linked)
        finally:
            self.git(self.source, "worktree", "remove", str(linked))

    def test_rejects_symlinked_object_storage_even_when_fsck_passes(self):
        objects = self.source / ".git/objects"
        outside = self.root / "outside-objects"
        objects.rename(outside)
        objects.symlink_to(outside, target_is_directory=True)
        self.git(self.source, "fsck", "--connectivity-only", "--no-dangling")
        with self.assertRaisesRegex(ValueError, "对象库"):
            SDK.verify_complete_repository(self.source)

    def test_accepts_normal_absorbed_submodule_gitfile(self):
        parent = self.root / "parent"
        parent.mkdir()
        self.git(parent, "init", "-q", "-b", "master")
        self.git(parent, "-c", "protocol.file.allow=always", "submodule", "add", "-q",
                 str(self.source), "module")
        module = parent / "module"
        self.assertTrue((module / ".git").is_file())
        SDK.verify_complete_repository(module, source_root=parent)

    def test_rejects_ignored_dirty_recursive_submodule(self):
        sdk = self.root / "sdk"
        framework = self.root / "framework"
        for path in (sdk, framework):
            path.mkdir()
            self.git(path, "init", "-q", "-b", "master")
            self.git(path, "config", "user.name", "SDK fixture")
            self.git(path, "config", "user.email", "sdk@example.invalid")
        self.git(framework, "-c", "protocol.file.allow=always", "submodule", "add", "-q",
                 str(self.source), "leaf")
        self.git(framework, "add", ".")
        self.git(framework, "commit", "-qm", "nested framework")
        for source, relative in ((self.source, "components/lwip/lwip"), (framework, "framework")):
            self.git(sdk, "-c", "protocol.file.allow=always", "submodule", "add", "-q",
                     str(source), relative)
        self.git(sdk, "add", ".")
        self.git(sdk, "commit", "-qm", "SDK fixture")
        sdk_revision = self.git(sdk, "rev-parse", "HEAD")
        (self.source / "source.c").write_text("corrected source\n")
        self.git(self.source, "add", ".")
        self.git(self.source, "commit", "-qm", "lwIP correction")
        corrected = self.git(self.source, "rev-parse", "HEAD")
        lwip = sdk / "components/lwip/lwip"
        self.git(lwip, "fetch", "-q", "origin")
        self.git(lwip, "checkout", "-q", "--detach", corrected)
        self.git(sdk, "-c", "protocol.file.allow=always", "submodule", "update", "--init", "--recursive")
        # submodule update resets lwIP; restore the single intentional override.
        self.git(lwip, "checkout", "-q", "--detach", corrected)
        lock = {"idf": {"revision": sdk_revision},
                "lwip": {"path": "components/lwip/lwip", "revision": corrected}}
        with patch.object(SDK, "LOCK", lock):
            SDK.check(sdk)
            self.git(sdk, "config", "submodule.framework.ignore", "all")
            self.git(sdk / "framework", "config", "submodule.leaf.ignore", "all")
            (sdk / "framework/leaf/source.c").write_text("unverified source\n")
            with self.assertRaises(ValueError):
                SDK.check(sdk)

    def test_rejects_environment_alternate_objects(self):
        with patch.dict(os.environ, {"GIT_ALTERNATE_OBJECT_DIRECTORIES":
                                    str(self.source / ".git/objects")}):
            with self.assertRaisesRegex(ValueError, "alternate"):
                SDK.verify_complete_repository(self.source)

    def test_rejects_environment_object_directory(self):
        with patch.dict(os.environ, {"GIT_OBJECT_DIRECTORY": str(self.source / ".git/objects")}):
            with self.assertRaisesRegex(ValueError, "alternate"):
                SDK.verify_complete_repository(self.source)

    def test_rejects_external_separate_git_directory_with_worktree_binding(self):
        separate = self.root / "separate"
        outside = self.root / "separate.git"
        self.git(self.root, "clone", "-q", "--separate-git-dir=" + str(outside),
                 str(self.source), str(separate))
        self.git(separate, "config", "core.worktree", str(separate))
        self.assertEqual(self.git(separate, "status", "--porcelain"), "")
        self.git(separate, "fsck", "--connectivity-only", "--no-dangling")
        with self.assertRaisesRegex(ValueError, "Git 元数据"):
            SDK.verify_complete_repository(separate)

    def test_rejects_git_environment_redirecting_metadata_and_worktree(self):
        alias = self.root / "environment-redirect"
        shutil.copytree(self.source, alias, ignore=shutil.ignore_patterns(".git"))
        with patch.dict(os.environ, {"GIT_DIR": str(self.source / ".git"),
                                    "GIT_WORK_TREE": str(alias)}):
            with self.assertRaisesRegex(ValueError, "Git 环境"):
                SDK.verify_complete_repository(alias)

    def test_rejects_replace_ref_even_when_head_and_status_match(self):
        original = self.git(self.source, "rev-parse", "HEAD")
        filename = self.source / "source.c"
        original_bytes = filename.read_bytes()
        filename.write_text("int substituted_business;\n")
        self.git(self.source, "add", filename.name)
        self.git(self.source, "commit", "-qm", "different source fixture")
        replacement = self.git(self.source, "rev-parse", "HEAD")
        self.git(self.source, "replace", original, replacement)
        self.git(self.source, "checkout", "-q", "--detach", original)
        self.assertEqual(self.git(self.source, "rev-parse", "HEAD"), original)
        self.assertEqual(self.git(self.source, "status", "--porcelain"), "")
        self.assertEqual(filename.read_text(), "int substituted_business;\n")
        # The verifier's ordinary Git reads ignore replacement refs even before rejection.
        self.assertEqual(SDK.git(self.source, "show", "HEAD:" + filename.name).encode(),
                         original_bytes.rstrip(b"\n"))
        with self.assertRaisesRegex(ValueError, "replace"):
            SDK.verify_complete_repository(self.source)

    def test_rejects_grafts_even_when_fsck_passes(self):
        head = self.git(self.source, "rev-parse", "HEAD")
        (self.source / ".git/info/grafts").write_text(head + "\n")
        self.git(self.source, "fsck", "--connectivity-only", "--no-dangling")
        with self.assertRaisesRegex(ValueError, "grafts"):
            SDK.verify_complete_repository(self.source)

    def test_rejects_absorbed_metadata_outside_own_root_modules(self):
        parent = self.root / "parent"
        parent.mkdir()
        self.git(parent, "init", "-q", "-b", "master")
        self.git(parent, "-c", "protocol.file.allow=always", "submodule", "add", "-q",
                 str(self.source), "module")
        module = parent / "module"
        SDK.verify_complete_repository(module, source_root=parent)
        directory = Path(self.git(module, "rev-parse", "--absolute-git-dir"))
        outside = self.root / "outside-module.git"
        directory.rename(outside)
        (module / ".git").write_text("gitdir: " + str(outside) + "\n")
        self.git(self.root, "config", "--file", str(outside / "config"),
                 "core.worktree", str(module))
        self.git(module, "fsck", "--connectivity-only", "--no-dangling")
        with self.assertRaisesRegex(ValueError, "Git 元数据"):
            SDK.verify_complete_repository(module, source_root=parent)


if __name__ == "__main__":
    unittest.main()
