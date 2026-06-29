import sys

with open('.github/workflows/build.yml', 'r') as f:
    content = f.read()

# Update Windows pyinstaller command
# From: pyinstaller --name "mobipeg-gui" --windowed --add-binary "$FFMPEG_BIN;." --distpath pyinst_dist encode_gui.py
# To: pyinstaller --name "mobipeg-gui" --windowed --add-binary "$FFMPEG_BIN;." --add-data "logo.png;." --icon=logo.png --distpath pyinst_dist encode_gui.py
content = content.replace(
    'pyinstaller --name "mobipeg-gui" --windowed --add-binary "$FFMPEG_BIN;." --distpath pyinst_dist encode_gui.py',
    'pyinstaller --name "mobipeg-gui" --windowed --add-binary "$FFMPEG_BIN;." --add-data "logo.png;." --icon=logo.png --distpath pyinst_dist encode_gui.py'
)

# Update macOS universal pyinstaller command
# From: pyinstaller --name "mobipeg-gui" --windowed --add-binary "$FFMPEG_BIN:." --distpath pyinst_dist --target-architecture universal2 encode_gui.py
# To: pyinstaller --name "mobipeg-gui" --windowed --add-binary "$FFMPEG_BIN:." --add-data "logo.png:." --icon=logo.png --distpath pyinst_dist --target-architecture universal2 encode_gui.py
content = content.replace(
    'pyinstaller --name "mobipeg-gui" --windowed --add-binary "$FFMPEG_BIN:." --distpath pyinst_dist --target-architecture universal2 encode_gui.py',
    'pyinstaller --name "mobipeg-gui" --windowed --add-binary "$FFMPEG_BIN:." --add-data "logo.png:." --icon=logo.png --distpath pyinst_dist --target-architecture universal2 encode_gui.py'
)

# Update Linux pyinstaller command
# From: pyinstaller --name "mobipeg-gui" --windowed --add-binary "$FFMPEG_BIN:." --distpath pyinst_dist encode_gui.py
# To: pyinstaller --name "mobipeg-gui" --windowed --add-binary "$FFMPEG_BIN:." --add-data "logo.png:." --icon=logo.png --distpath pyinst_dist encode_gui.py
content = content.replace(
    'pyinstaller --name "mobipeg-gui" --windowed --add-binary "$FFMPEG_BIN:." --distpath pyinst_dist encode_gui.py',
    'pyinstaller --name "mobipeg-gui" --windowed --add-binary "$FFMPEG_BIN:." --add-data "logo.png:." --icon=logo.png --distpath pyinst_dist encode_gui.py'
)

with open('.github/workflows/build.yml', 'w') as f:
    f.write(content)
