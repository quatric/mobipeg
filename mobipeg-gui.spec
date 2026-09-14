# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['encode_gui.py'],
    pathex=[],
    binaries=[
        ('ffmpeg', '.'),
        ('ffprobe', '.'),
        ('ffplay', '.'),
    ],
    datas=[('logo.png', '.')],
    hiddenimports=['encode'],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=['PIL'],
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
    icon=['logo.ico' if sys.platform.startswith('win') else 'logo.icns' if sys.platform == 'darwin' else 'logo.png'],
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
    icon='logo.icns',
    bundle_identifier=None,
    version='2.0',
)
