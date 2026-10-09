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
