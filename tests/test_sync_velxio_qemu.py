"""Offline tests; no network or secrets needed."""
import importlib.util
import io
import tarfile
import tempfile
from pathlib import Path
import unittest

ROOT = Path(__file__).parents[1]
spec = importlib.util.spec_from_file_location('sync_velxio_qemu', ROOT / 'maintenance/sync_velxio_qemu.py')
sync = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sync)


def archive(path, evil_link=False):
    files = {
        'upstream-1234/build_libqemu-esp32.sh': b'build',
        'upstream-1234/configure': b'config',
        'upstream-1234/meson.build': b'project()',
        'upstream-1234/hw/i2c/peripheral.c': b'int i = 1;',
        'upstream-1234/.github/workflows/evil.yml': b'permissions: write-all',
    }
    with tarfile.open(path, 'w:gz') as tf:
        for filename, content in files.items():
            info = tarfile.TarInfo(filename)
            info.size = len(content)
            tf.addfile(info, io.BytesIO(content))
        if evil_link:
            link = tarfile.TarInfo('upstream-1234/danger')
            link.type = tarfile.SYMTYPE
            link.linkname = '../../outside'
            tf.addfile(link)


class SourceTests(unittest.TestCase):
    def test_array_manifest(self):
        result = sync.manifest_assets({'assets': [{'name': 'qemu-source-abcdef12',
                    'sha256': 'a' * 64, 'created_at': '2026-10-26'}]})
        self.assertEqual(result[0]['name'], 'qemu-source-abcdef12')
        self.assertIsNotNone(result[0]['date'])

    def test_dict_manifest(self):
        result = sync.manifest_assets({'assets': {'qemu-source-abcdef12': {'sha256': 'b' * 64}}})
        self.assertEqual(result[0]['sha256'], 'b' * 64)

    def test_invalid_sha_rejected(self):
        with self.assertRaises(ValueError):
            sync.manifest_assets({'assets': [{'name': 'qemu-source-abcdef12', 'sha256': 'not-a-hash'}]})

    def test_traversal_rejected(self):
        with self.assertRaises(ValueError):
            sync.parts_for('../secrets')

    def test_extract_keeps_actions_protected(self):
        with tempfile.TemporaryDirectory() as d:
            tarpath, stage = Path(d) / 'file.tar.gz', Path(d) / 'stage'
            stage.mkdir()
            archive(tarpath)
            files = sync.unpack(tarpath, stage)
            self.assertIn('hw/i2c/peripheral.c', files)
            self.assertNotIn('.github/workflows/evil.yml', files)
            self.assertFalse((stage / '.github').exists())

    def test_symlink_escape_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            tarpath, stage = Path(d) / 'file.tar.gz', Path(d) / 'stage'
            stage.mkdir()
            archive(tarpath, evil_link=True)
            with self.assertRaises(ValueError):
                sync.unpack(tarpath, stage)


if __name__ == '__main__':
    unittest.main()
