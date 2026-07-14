# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['encode_gui.py'],
    pathex=[],
    binaries=[],
    datas=[('logo.png', '.'), ('rvid.py', '.'), ('rvid_lz.c', '.')],
    hiddenimports=['encode', 'rvid', 'numpy'],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='mobipeg-gui',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=['logo.png'],
)
coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=True,
    upx_exclude=[],
    name='mobipeg-gui',
)
app = BUNDLE(
    coll,
    name='mobipeg-gui.app',
    icon='logo.png',
    bundle_identifier=None,
    version='1.1',
)
