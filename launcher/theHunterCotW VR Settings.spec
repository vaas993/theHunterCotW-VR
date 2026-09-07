# -*- mode: python ; coding: utf-8 -*-
#
# FOLDER MODE, AND NO UPX - both for the same reason: false positives.
#
# This was a one-file, UPX-compressed build, and those are the two things that
# make antivirus heuristics treat a harmless Python program as a dropper:
#
#   one-file  the exe writes its own contents into %TEMP% and runs them, which
#             is behaviourally what a dropper does. It is also how a great deal
#             of real malware is delivered, so the machine-learning engines have
#             learned the shape rather than the intent.
#   UPX       a compressed executable hides its own imports and strings until it
#             runs. Legitimate software rarely bothers; packers are the norm in
#             malware, so "packed" alone moves the needle.
#
# Measured before the change: 5 of 66 engines flagged the package, one of them
# naming XWorm - a family commonly delivered as exactly this, a packed one-file
# bundle inside an archive. None of the major engines flagged it.
#
# The cost is that the launcher ships as a FOLDER rather than a single exe. That
# is a real cost for a mod people unzip by hand, and it is worth paying: an
# antivirus warning stops far more people from using this than an extra folder
# ever will.

a = Analysis(
    ['cotwvr_launcher.py'],
    pathex=[],
    binaries=[],
    datas=[],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=['numpy', 'PIL', 'matplotlib', 'scipy', 'pandas'],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,          # the rest goes beside it, not inside it
    name='theHunterCotW VR Settings',
    version='version_info.txt',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)

coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=False,
    upx_exclude=[],
    name='VR Settings',
)
