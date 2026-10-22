# os.path for /bin/python: POSIX paths, same results as CPython's posixpath
import os
sep = "/"

def isabs(p):
    return p.startswith("/")

def join(a, *p):
    for b in p:
        if b.startswith("/"):
            a = b
        elif not a or a.endswith("/"):
            a += b
        else:
            a += "/" + b
    return a

def split(p):
    i = p.rfind("/") + 1
    head = p[:i]
    if head and head != "/" * len(head):
        head = head.rstrip("/")
    return head, p[i:]

def basename(p):
    return p[p.rfind("/") + 1:]

def dirname(p):
    return split(p)[0]

def splitext(p):
    s, d = p.rfind("/"), p.rfind(".")
    if d > s:
        i = s + 1
        while i < d:                    # leading dots belong to the name: ".bashrc" has no extension
            if p[i] != ".":
                return p[:d], p[d:]
            i += 1
    return p, ""

def normpath(p):
    if not p:
        return "."
    slashes = 2 if p.startswith("//") and not p.startswith("///") else 1 if p.startswith("/") else 0
    out = []
    for c in p.split("/"):
        if c in ("", "."):
            continue
        if c != ".." or (not slashes and not out) or (out and out[-1] == ".."):
            out.append(c)
        elif out:
            out.pop()
    return "/" * slashes + "/".join(out) or "."

def abspath(p):
    return normpath(p if p.startswith("/") else join(os.getcwd(), p))

def expanduser(p):
    if p == "~" or p.startswith("~/"):
        return (os.getenv("HOME", "/home").rstrip("/") + p[1:]) or "/"
    return p

def _mode(p):
    try:
        return os.stat(p)[0] & 0o170000
    except OSError:
        return -1

def exists(p):
    return _mode(p) >= 0

def isdir(p):
    return _mode(p) == 0o040000

def isfile(p):
    return _mode(p) == 0o100000

def getsize(p):
    return os.stat(p)[6]

def getmtime(p):
    return os.stat(p)[8]
