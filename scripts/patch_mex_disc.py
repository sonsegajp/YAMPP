"""Apply the shared read-only disc importer to the pinned m-ex checkout."""
from pathlib import Path
import hashlib,json
ROOT=Path(__file__).resolve().parents[1]
def patch_checkout():
    target=ROOT/'upstream/MexManager/mexLib/MexWorkspace.cs'
    raw=target.read_bytes(); digest=hashlib.sha256(raw).hexdigest()
    expected=json.loads((ROOT/'scripts/patches/mex-shared-disc.json').read_text())
    if digest not in expected.values():raise ValueError('MexWorkspace differs from the pinned original/patched source; preserve local changes')
    if digest==expected['original']:
        text=raw.decode('utf-8-sig')
        start=text.index('            using (GCISO iso = new(isoPath))')
        end=text.index('            // create data and asset directories',start)
        data=(text[:start]+'            YamppDiscReader.Extract(isoPath, sys, files);\r\n\r\n'+text[end:]).encode('utf-8')
        if raw.startswith(b'\xef\xbb\xbf'):data=b'\xef\xbb\xbf'+data
        if hashlib.sha256(data).hexdigest()!=expected['patched']:raise ValueError('m-ex patch verification failed')
        target.write_bytes(data)
    helper=target.with_name('YamppDiscReader.cs'); data=(ROOT/'scripts/patches/mex-shared-disc.cs').read_bytes()
    if helper.exists() and helper.read_bytes()!=data:raise ValueError('Existing m-ex disc reader differs; preserve local changes')
    helper.write_bytes(data)
if __name__=='__main__':
    patch_checkout();print('m-ex shared-read disc importer ready')
