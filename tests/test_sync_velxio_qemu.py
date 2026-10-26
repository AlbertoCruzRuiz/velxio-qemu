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
    def test_velxio_api_asset_name_and_version_date(self):
        # Actual API field names and intentionally UNSORTED source releases.
        manifest = {
            'plan': 'personal',
            'assets': [
                {'asset_name': 'esp32s3_rev0_rom', 'version': '1.0.0',
                 'size_bytes': 393216, 'sha256': 'b' * 64},
                {'asset_name': 'qemu-source-1c2c5b3d',
                 'version': '2026-10-14+1c2c5b3d',
                 'size_bytes': 41726664,
                 'sha256': '612e6055959aaff94d6af40de9992f5c8d4834dfc0560351f930606d4ec31a0b'},
                {'asset_name': 'qemu-source-489079d3',
                 'version': '2026-06-26+489079d3',
                 'size_bytes': 38444604,
                 'sha256': 'f93108912d6fa57ac4aa2a6630b3bac8424065e48a584802839c212e1acd90b9'},
                {'asset_name': 'qemu-source-63b37f5e',
                 'version': '2026-10-15+63b37f5e',
                 'size_bytes': 41726819,
                 'sha256': 'f36f04e8e156211783db0bfd408db66450e170df536f8f893f58ef09743cfe39'},
                {'asset_name': 'qemu-source-ea9be605',
                 'version': '2026-09-27+ea9be605',
                 'size_bytes': 41725647,
                 'sha256': 'f25d44ca498e0d6a757c0d7fd451f16c14942f9dcef62bcf815acbe72a6848cc'},
            ]
        }
        result = sync.manifest_assets(manifest)
        self.assertEqual(len(result), 4)
        newest = sync.latest(result, '')
        self.assertEqual(newest['name'], 'qemu-source-63b37f5e')
        self.assertEqual(newest['size_bytes'], 41726819)
        self.assertEqual(newest['sha256'],
                         'f36f04e8e156211783db0bfd408db66450e170df536f8f893f58ef09743cfe39')

    def test_version_commit_mismatch_rejected(self):
        with self.assertRaisesRegex(ValueError, 'version/commit mismatch'):
            sync.manifest_assets({'assets': [
                {'asset_name': 'qemu-source-63b37f5e',
                 'version': '2026-10-15+1c2c5b3d', 'sha256': 'a' * 64}
            ]})

    def test_no_implicit_selection_if_date_unknown(self):
        sources = sync.manifest_assets({'assets': [
            {'asset_name': 'qemu-source-63b37f5e', 'sha256': 'a' * 64},
            {'asset_name': 'qemu-source-1c2c5b3d',
             'version': '2026-10-14+1c2c5b3d', 'sha256': 'b' * 64}
        ]})
        with self.assertRaisesRegex(ValueError, 'release date'):
            sync.latest(sources, '')
        self.assertEqual(sync.latest(sources, 'qemu-source-63b37f5e')['name'],
                         'qemu-source-63b37f5e')

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

class BootstrapTests(unittest.TestCase):
    def test_empty_repository_imports_initial_source(self):
        """An empty source tree must create files and mark changed=true."""
        import json
        import hashlib
        import os
        from unittest.mock import patch

        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            source = root / 'upstream.tar.gz'
            archive(source)
            digest = hashlib.sha256(source.read_bytes()).hexdigest()
            manifest = {'plan': 'personal', 'assets': [
                {'asset_name': 'qemu-source-1c2c5b3d',
                 'sha256': 'a' * 64, 'version': '2026-10-14+1c2c5b3d'},
                {'asset_name': 'qemu-source-63b37f5e',
                 'sha256': digest, 'size_bytes': source.stat().st_size,
                 'version': '2026-10-15+63b37f5e'}]}
            results = []

            def fake_api_get(name, key, dest):
                if name is None:
                    dest.write_text(json.dumps(manifest))
                else:
                    self.assertTrue(name.startswith('qemu-source-'))
                    dest.write_bytes(source.read_bytes())

            prev = Path.cwd()
            try:
                os.chdir(root)
                with patch.object(sync, 'api_get', side_effect=fake_api_get), \
                     patch.object(sync, 'emit', side_effect=lambda k,v: results.append((k,v))), \
                     patch.dict(os.environ, {'VELXIO_LICENSE_KEY': 'redacted-test-key',
                                              'SOURCE_ASSET': ''}):
                    sync.main()
                    self.assertTrue((root / 'build_libqemu-esp32.sh').is_file())
                    self.assertEqual(json.loads((root / 'upstream-version.json').read_text())['qemu_commit'], '63b37f5e')
                    self.assertIn(('changed', 'true'), results)
                    results.clear()
                    sync.main()
                    self.assertIn(('changed', 'false'), results)
                    (root / 'hw/i2c/peripheral.c').write_text('custom patch')
                    manifest['assets'].insert(0, {'asset_name': 'qemu-source-ffffffff', 'sha256': digest,
                                                 'version': '2026-10-16+ffffffff'})
                    with self.assertRaisesRegex(ValueError, 'Local QEMU source differs'):
                        sync.main()
            finally:
                os.chdir(prev)
