"""A service crashing at shutdown must make its acceptance test fail."""
import pathlib
import signal
import tempfile
import unittest
from wire import Server


class HarnessTest(unittest.TestCase):
    def script(self, directory, exit_code):
        path = pathlib.Path(directory) / "child.py"
        path.write_text("#!/usr/bin/env python3\nimport signal,sys,time\n"
                        "def stop(*_):\n print('shutdown-marker',file=sys.stderr,flush=True)\n sys.exit("+str(exit_code)+")\n"
                        "signal.signal(signal.SIGTERM,stop)\nprint('LISTENING 12345',flush=True)\n"
                        "while True: time.sleep(1)\n", encoding="utf-8")
        path.chmod(0o700)
        return path

    def test_nonzero_shutdown_fails_and_retains_diagnostic(self):
        with tempfile.TemporaryDirectory() as directory:
            server = Server(self.script(directory, 7))
            with self.assertRaisesRegex(AssertionError, "shutdown-marker"):
                server.close()

    def test_intentional_kill_is_separate_from_graceful_shutdown(self):
        with tempfile.TemporaryDirectory() as directory:
            server = Server(self.script(directory, 0))
            server.close(force=True)
            self.assertEqual(server.process.returncode, -signal.SIGKILL)


if __name__ == "__main__":
    unittest.main()
