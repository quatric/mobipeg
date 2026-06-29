with open('.github/workflows/build.yml', 'r') as f:
    content = f.read()

# Replace the linux configure command
old_configure = """          ./configure \\
            --prefix="$PWD/dist" \\
            --arch=${{ matrix.ffarch }} \\
            --enable-gpl \\
            --enable-libx264 \\
            --enable-libvorbis \\
            --disable-doc \\
            --disable-debug \\
            --pkg-config-flags="--static" \\
            --extra-cflags="-I$DEPS/include" \\
            --extra-ldflags="-L$DEPS/lib" \\"""

new_configure = """          ./configure \\
            --prefix="$PWD/dist" \\
            --arch=${{ matrix.ffarch }} \\
            --enable-gpl \\
            --enable-libx264 \\
            --enable-libvorbis \\
            --disable-doc \\
            --disable-debug \\
            --pkg-config-flags="--static" \\
            --extra-cflags="-I$DEPS/include" \\
            --extra-ldflags="-L$DEPS/lib" \\
            --extra-libs="-logg -lm -lstdc++" \\"""

# I need to double check the exact string before replacing.
