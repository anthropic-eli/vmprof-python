import gzip
import os
import sys
import tempfile

import requests

try:
    from urlparse import urlparse
except ImportError:
    # python 3
    from urllib.parse import urlparse


def compress_file(filename):
    fileno, name = tempfile.mkstemp(prefix="jit", suffix=".log.zip")
    os.close(fileno)
    with open(filename, "rb") as fd:
        with gzip.open(name, "wb") as zipfd:
            while True:
                chunk = fd.read(1024)
                if not chunk:
                    break
                zipfd.write(chunk)
    return name


def upload(filepath, url):
    zfilepath = compress_file(filepath)
    with open(zfilepath, "rb") as fd:
        r = requests.post(url, files={"file": fd})
        if r.status_code != 200:
            sys.stderr.write(
                "PyPy JIT log: Server rejected file. status: %d, msg: '%s'\n"
                % (r.status_code, r.text)
            )
            return
        checksum = r.text[1:-1]
        assert checksum != ""
        netloc = urlparse(url).netloc
        sys.stderr.write(f"PyPy JIT log: http://{netloc}/#/{checksum}/traces\n")
