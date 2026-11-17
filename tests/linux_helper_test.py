"""Tests the build helper's request boundary and real subprocess results."""
import importlib.util
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
(ROOT / "tests/results").mkdir(exist_ok=True)
spec = importlib.util.spec_from_file_location("linux_verify", ROOT / "tests/linux_verify.py")
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


class LinuxHelperTest(unittest.TestCase):
    def test_build_test_generates_fixed_cmake_and_ctest_commands(self):
        commands = helper.command_plan({"id": "trial-1", "action": "build_test"}, ROOT)
        self.assertEqual([command[0] for command in commands], ["cmake", "cmake", "ctest"])
        self.assertIn("-DQTCHAT_BUILD_CLIENT=OFF", commands[0])
        self.assertEqual(commands[-1][-1], "--output-on-failure")

    def test_arbitrary_programs_paths_and_shell_actions_are_rejected(self):
        for request in [
            {"id": "../outside", "action": "test"},
            {"id": "trial", "action": "shell", "command": "echo unsafe"},
            {"id": "trial", "action": "build", "client": "yes"},
        ]:
            with self.subTest(request=request), self.assertRaises(ValueError):
                helper.command_plan(request, ROOT)

    def test_child_failure_is_logged_and_stops_later_commands(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "tests/results", ignore_cleanup_errors=True) as directory:
            log = pathlib.Path(directory) / "run.log"
            status = helper.run_commands([
                [sys.executable, "-c", "print('specific-failure'); raise SystemExit(7)"],
                [sys.executable, "-c", "print('must-not-run')"],
            ], log, ROOT)
            self.assertEqual(status, 7)
            self.assertIn("specific-failure", log.read_text())
            self.assertNotIn("must-not-run", log.read_text())

    def test_success_and_missing_executable_have_truthful_results(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "tests/results", ignore_cleanup_errors=True) as directory:
            log = pathlib.Path(directory) / "run.log"
            self.assertEqual(helper.run_commands([[sys.executable, "-c", "print('ok')"]], log, ROOT), 0)
            self.assertIn("ok", log.read_text())
            self.assertEqual(helper.run_commands([["qtchat-nonexistent-program-153"]], log, ROOT), 127)


if __name__ == "__main__":
    unittest.main()
