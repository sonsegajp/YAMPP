import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import upstream_download as ud


class Response(io.BytesIO):
    status=200
    def __init__(self,data, url="https://release-assets.githubusercontent.com/test", length=None):
        super().__init__(data);self.url=url;self.headers={} if length is None else {"Content-Length":str(length)}
    def geturl(self):return self.url


class UpstreamDownloadTests(unittest.TestCase):
    def setUp(self):
        (ud.ROOT/"build/tests").mkdir(parents=True,exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(dir=ud.ROOT/"build/tests");self.folder=Path(self.temp.name)
        self.value=ud.identity(ud.BUILDS[0]);self.data=b"verified archive fixture"
        self.spec={"name":"test.7z","url":"https://github.com/akaneia/akaneia-build/releases/download/test/file", "size":len(self.data),"sha256":hashlib.sha256(self.data).hexdigest()}
    def tearDown(self):self.temp.cleanup()
    def fetch(self,data,**kw):
        with patch.object(ud,"build_opener") as opener:
            opener.return_value.open.return_value=Response(data,**kw)
            return ud._download(self.folder,self.spec)
    def test_no_confirmation_never_touches_network_or_disk(self):
        with patch.object(ud,"_directory") as directory,patch.object(ud,"build_opener") as network:
            with self.assertRaisesRegex(ValueError,"Confirm"):ud.acquire(self.value)
            directory.assert_not_called();network.assert_not_called()
    def test_pin_and_native_constants_match(self):
        header=(ud.ROOT/"native/host/gxrt/upstream_build.h").read_text()
        self.assertIn(self.value["sha256"],header);self.assertIn(self.value["version"],header)
        self.assertFalse(ud.plan(self.value)["runtimeSupported"])
    def test_complete_file_verified_before_promotion(self):
        target=self.fetch(self.data,length=len(self.data));self.assertEqual(target.read_bytes(),self.data)
        with patch.object(ud,"build_opener") as opener:
            self.assertEqual(ud._download(self.folder,self.spec),target);opener.assert_not_called()
    def test_bad_hash_short_and_oversize_never_install(self):
        for data in (b"x"*len(self.data),self.data[:-1],self.data+b"!"):
            with self.assertRaises(ValueError):self.fetch(data)
            self.assertFalse((self.folder/"test.7z").exists());self.assertFalse(list(self.folder.glob("*.part")))
    def test_unexpected_response_length_never_install(self):
        with self.assertRaises(ValueError):self.fetch(self.data,length=1)
        self.assertFalse((self.folder/"test.7z").exists())
    def test_network_failure_preserves_previous_file(self):
        target=self.folder/"test.7z";target.write_bytes(b"old")
        with patch.object(ud,"build_opener") as opener:
            opener.return_value.open.side_effect=TimeoutError()
            with self.assertRaises(TimeoutError):ud._download(self.folder,self.spec)
        self.assertEqual(target.read_bytes(),b"old");self.assertFalse(list(self.folder.glob("*.part")))
    def test_redirects_stay_on_github_https(self):
        for url in ("http://github.com/test","https://github.com.evil.test/a","https://127.0.0.1/a","file:///data/mod","https://user@github.com/a","https://github.com:8443/a"):
            with self.assertRaises(ValueError):ud._url(url)
        with self.assertRaises(ValueError):self.fetch(self.data,url="https://example.com/mod")
    def test_corrupt_cache_is_reverified(self):
        target=self.folder/"test.7z";target.write_bytes(b"x"*len(self.data))
        self.assertFalse(ud._verified(target,self.spec));self.fetch(self.data);self.assertTrue(ud._verified(target,self.spec))
    def test_two_downloads_cannot_promote_at_once(self):
        with ud._lock(self.folder):
            with self.assertRaises(ValueError):
                with ud._lock(self.folder):pass

if __name__=="__main__":unittest.main()
