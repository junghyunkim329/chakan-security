import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
MODULE = ROOT / "ECU/tools/security/stage_vsomeip_runtime.py"


class VsomeipRuntimeTest(unittest.TestCase):
    def _load_module(self):
        spec = importlib.util.spec_from_file_location("stage_vsomeip_runtime", MODULE)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_stages_modules_loaded_at_runtime(self):
        module = self._load_module()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "source"
            boost_source = root / "boost"
            destination = root / "destination"
            source.mkdir()
            boost_source.mkdir()
            for name in module.REQUIRED_MODULES:
                (source / name).write_bytes(name.encode())
            boost_name = "libboost_filesystem.so.1.90.0"
            (boost_source / boost_name).write_bytes(boost_name.encode())

            module.stage(source, boost_source, destination)

            self.assertEqual(
                {path.name for path in destination.iterdir()},
                {*module.REQUIRED_MODULES, boost_name},
            )
            for name in module.REQUIRED_MODULES:
                self.assertEqual((destination / name).read_bytes(), name.encode())
            self.assertEqual((destination / boost_name).read_bytes(), boost_name.encode())

    def test_rejects_incomplete_runtime(self):
        module = self._load_module()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "source"
            boost_source = root / "boost"
            source.mkdir()
            boost_source.mkdir()
            (source / "libvsomeip3.so.3").write_bytes(b"core")
            (boost_source / "libboost_filesystem.so.1.90.0").write_bytes(b"boost")

            with self.assertRaisesRegex(FileNotFoundError, "libvsomeip3-cfg.so.3"):
                module.stage(source, boost_source, root / "destination")


if __name__ == "__main__":
    unittest.main()
