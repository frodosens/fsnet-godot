"""Keep the editor-only upstream diagnostic exception narrowly scoped."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("integration_tools", Path(__file__).resolve().parents[1] / "tools/run_integration.py")
integration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(integration)

DIAGNOSTIC = ('ERROR: EditorSettings not instantiated yet when getting setting "export/android/android_sdk_path".\n'
              '   at: _EDITOR_GET (editor/settings/editor_settings.cpp:1531)\n')


class EditorDiagnosticTests(unittest.TestCase):
    def invoke(self, output, editor=True, exit_code=0):
        command = ["godot", "--editor"] if editor else ["godot"]
        with tempfile.TemporaryDirectory() as folder:
            log = Path(folder) / "godot.log"
            with patch.object(integration.subprocess, "run", return_value=subprocess.CompletedProcess(command, exit_code, output, "")):
                return integration.run(command, folder, {}, log)

    def test_exact_editor_shutdown_diagnostic_preserves_raw_log(self):
        self.assertEqual(self.invoke(DIAGNOSTIC), DIAGNOSTIC)

    def test_exception_does_not_hide_game_script_or_native_failures(self):
        cases = [(DIAGNOSTIC, False, 0), (DIAGNOSTIC, True, 1),
                 (DIAGNOSTIC + "SCRIPT ERROR: broken script\n", True, 0),
                 (DIAGNOSTIC + "ERROR: native extension failed to load\n", True, 0),
                 (DIAGNOSTIC.replace("_EDITOR_GET", "other_location"), True, 0)]
        for output, editor, exit_code in cases:
            with self.subTest(output=output, editor=editor, exit_code=exit_code):
                with self.assertRaises(RuntimeError):
                    self.invoke(output, editor, exit_code)
