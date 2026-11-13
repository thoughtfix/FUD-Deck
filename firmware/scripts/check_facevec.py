# PlatformIO pre-script: warns when the face art changed but facevec_data.h was not re-traced.
# Standard library only (the tracer itself needs numpy/scipy/scikit-image, which PlatformIO's
# Python does not ship, so facevec_data.h is generated with tools/facevec/run.sh and committed).
#
# platformio.ini:   extra_scripts = pre:scripts/check_facevec.py
# Paths below assume this file sits in firmware/scripts/ and the header in firmware/src/ui/.
import hashlib, os, re

try:
    BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
except NameError:
    BASE = os.getcwd()   # PlatformIO pre-script (SCons): cwd is the project dir

ART = os.path.join(BASE, "tools", "facevec", "art")
HEADER = os.path.join(BASE, "src", "ui", "facevec_data.h")


def main():
    try:
        with open(HEADER) as fh:
            top = fh.read(3000)
    except OSError:
        print("check_facevec: %s not found" % HEADER)
        return
    m = re.search(r"source-sha1: ([0-9a-f]{40})", top)
    names = re.search(r"source-faces: (\S+)", top)
    if not names:
        return
    h = hashlib.sha1()
    for name in names.group(1).split(","):
        p = os.path.join(ART, name + ".png")
        if not os.path.exists(p):
            return   # the full art is not here (only dropped in to re-trace): nothing to check
        with open(p, "rb") as fh:
            h.update(name.encode() + b"\0" + fh.read())
    if not m or m.group(1) != h.hexdigest():
        print("check_facevec: WARNING face art changed since facevec_data.h was traced. "
              "Run tools/facevec/run.sh.")


main()
