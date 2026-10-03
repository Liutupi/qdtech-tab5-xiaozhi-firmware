"""Guard the Tab5 AFE consumer scheduling policy without requiring ESP-IDF."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


class AfeTaskPriorityTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
    def test_tab5_fetch_preempts_display_but_not_input(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / 'main/audio/engines/afe_audio_engine.cc').read_text()
        input_source = (root / 'main/audio/audio_service.cc').read_text()
        self.assertRegex(source, re.compile(
            r'"audio_afe"\s*,\s*kProcessingTaskStackSize\s*,\s*this\s*,\s*'
            r'kAfeProcessingTaskPriority', re.S))
        self.assertRegex(input_source, re.compile(r'"audio_input"\s*,.*?this\s*,\s*8\s*,', re.S))

        start = source.index('#if CONFIG_BOARD_TYPE_QDTECH_TAB5\n// The Tab5 display task')
        end = source.index('#endif', start) + len('#endif')
        policy = source[start:end]
        with tempfile.TemporaryDirectory() as directory:
            source_file = Path(directory) / 'priority.cc'
            binary_file = Path(directory) / 'priority'
            for tab5, expected in ((1, 5), (0, 3)):
                source_file.write_text(
                    'using UBaseType_t = unsigned;\n' + policy + '\n'
                    f'static_assert(kAfeProcessingTaskPriority == {expected});\n'
                    'static_assert(kAfeProcessingTaskPriority < 8);\n'
                    'int main() {}\n')
                subprocess.run(
                    ['c++', '-std=c++17', f'-DCONFIG_BOARD_TYPE_QDTECH_TAB5={tab5}',
                     str(source_file), '-o', str(binary_file)],
                    check=True, capture_output=True, text=True)


if __name__ == '__main__':
    unittest.main()
