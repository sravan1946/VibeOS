#!/usr/bin/env python3
"""Host check of user/bin/md5sum.c and sha256sum.c against hashlib (the programs only need vutil.h, so they build natively)."""
import hashlib, os, subprocess, sys, tempfile
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
with tempfile.TemporaryDirectory() as t:
    os.mkdir(t + "/inc")
    open(t + "/inc/vutil.h", "w").write(open(root + "/user/include/vutil.h").read())
    bad = 0
    for tool, h in (("md5sum", hashlib.md5), ("sha256sum", hashlib.sha256)):
        subprocess.check_call(["gcc", "-O1", "-w", "-I" + t + "/inc", root + "/user/bin/%s.c" % tool, "-o", t + "/" + tool])
        for n in list(range(130)) + [1000, 4095, 4096, 4097, 100000]:
            d = os.urandom(n)
            open(t + "/f", "wb").write(d)
            o = subprocess.run([t + "/" + tool, t + "/f"], capture_output=True, text=True).stdout.split()[0]
            if o != h(d).hexdigest(): bad += 1; print("BAD", tool, n)
        out = subprocess.run([t + "/" + tool, t + "/f"], capture_output=True, text=True).stdout
        open(t + "/sums", "w").write(out)
        if subprocess.run([t + "/" + tool, "-c", t + "/sums"], capture_output=True).returncode: bad += 1; print("BAD -c", tool)
    print("hashtest: %s" % ("FAILED %d" % bad if bad else "ok (md5sum, sha256sum match hashlib)"))
    sys.exit(bad != 0)
