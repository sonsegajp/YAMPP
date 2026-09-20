"""PNG-only previews never install or download a costume package."""
import hashlib
import io
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import zlib

import community


def chunk(kind, payload):
    return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload) & 0xffffffff)


def png(width=2, height=2, data=None):
    pixels = data if data is not None else (b'\0' + b'\xff\0\0\xff' * width) * height
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b''))


class Preview(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.raw = png()
        self.sha = 'a' * 64
        self.image_sha = hashlib.sha256(self.raw).hexdigest()
        self.metadata = dict(sha256=self.sha, kind='costume', additive=True, preview_path='https://untrusted.invalid/anything',
                             preview_sha256=self.image_sha, preview_size=len(self.raw))

    def response(self, raw=None, length=None):
        body = self.raw if raw is None else raw
        result = io.BytesIO(body)
        result.headers = {'Content-Length': str(len(body) if length is None else length)}
        return result

    def test_only_thumbnail_and_cache_integrity(self):
        with patch.object(community, 'PREVIEW_ROOT', Path(self.temp.name)), \
                patch.object(community, 'json_request', return_value=self.metadata) as metadata, \
                patch.object(community, 'request', side_effect=lambda *a, **k: self.response()) as request, \
                patch.object(community, 'download_verified', side_effect=AssertionError('ZIP downloaded')), \
                patch.object(community, 'costume_registry', side_effect=AssertionError('registry changed')):
            result = community.preview(self.sha)
            self.assertTrue(result['previewAvailable'])
            self.assertEqual((result['width'], result['height']), (2, 2))
            self.assertEqual(Path(result['previewPath']).read_bytes(), self.raw)
            self.assertEqual(result['previewSha256'], self.image_sha)
            request.assert_called_once_with('/' + self.sha + '/preview.png', accept='image/png')
            self.assertEqual(community.preview(self.sha), result)
            self.assertEqual(request.call_count, 1)
            self.assertEqual(metadata.call_count, 2)
            Path(result['previewPath']).write_bytes(self.raw[:-1] + b'X')
            with self.assertRaisesRegex(ValueError, 'integrity'):
                community.preview(self.sha)

    def test_absent_art_does_not_download(self):
        metadata = dict(sha256=self.sha, kind='costume', additive=True)
        with patch.object(community, 'json_request', return_value=metadata), patch.object(community, 'request') as request:
            self.assertEqual(community.preview(self.sha), {'sha256': self.sha, 'previewAvailable': False})
            request.assert_not_called()

    def test_bad_hash_and_response_size_rejected(self):
        with patch.object(community, 'PREVIEW_ROOT', Path(self.temp.name)), patch.object(community, 'json_request', return_value=self.metadata):
            with patch.object(community, 'request', return_value=self.response(self.raw[:-1] + b'X')):
                with self.assertRaisesRegex(ValueError, 'integrity'):
                    community.preview(self.sha)
            with patch.object(community, 'request', return_value=self.response(length=len(self.raw) + 1)):
                with self.assertRaisesRegex(ValueError, 'size'):
                    community.preview(self.sha)
        self.assertFalse(any(Path(self.temp.name).iterdir()))

    def test_metadata_bounds_and_package_binding(self):
        for changes in [{'sha256': 'b' * 64}, {'preview_size': community.MAX_PREVIEW + 1}, {'preview_size': True}, {'preview_sha256': 'invalid'}]:
            with patch.object(community, 'json_request', return_value=self.metadata | changes), patch.object(community, 'request') as request:
                with self.assertRaises(ValueError):
                    community.preview(self.sha)
                request.assert_not_called()

    def test_png_structure_compression_and_bombs(self):
        for raw in [self.raw + b'extra', self.raw[:-3], self.raw[:32] + b'X' + self.raw[33:],
                    png(data=b'\0' * 5000), png(4097, 1),
                    b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 2, 8, 6, 0, 0, 0)) + chunk(b'IDAT', b'bad') + chunk(b'IEND', b'')]:
            with self.assertRaises(ValueError):
                community.preview_png(raw)


if __name__ == '__main__':
    unittest.main()
