"""用真实 Git 对象库验证 SDK 来源不得借用外部对象。"""
import importlib.util
import os
from pathlib import Path
import subprocess
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
        SDK.verify_complete_repository(module)

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


if __name__ == "__main__":
    unittest.main()
