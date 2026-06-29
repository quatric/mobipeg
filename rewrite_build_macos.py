import sys
import re

with open('.github/workflows/build.yml', 'r') as f:
    content = f.read()

# 1. Replace the build-macos and build-macos-universal jobs
# We find the start of build-macos and the end of build-macos-universal.
start_idx = content.find('  build-macos:\n')
end_idx = content.find('  # ─────────────────────────────────────────────────────────────────────────\n  # Run test_roundtrip.sh')

new_build_macos = """  build-macos:
    runs-on: macos-14

    steps:
      - uses: actions/checkout@v4

      - name: Install dependencies
        run: |
          brew install nasm yasm pkg-config

      - name: Cache dependencies
        id: cache-deps
        uses: actions/cache@v4
        with:
          path: ${{ github.workspace }}/deps
          key: deps-macos-universal-v7

      - name: Build dependencies
        if: steps.cache-deps.outputs.cache-hit != 'true'
        run: |
          DEPS="${GITHUB_WORKSPACE}/deps"
          
          build_arch() {
            local ARCH=$1
            local TARGET_FLAG=$2
            local HOST_FLAG=$3
            local PREFIX="$DEPS/$ARCH"
            mkdir -p "$PREFIX"
            
            cd "$GITHUB_WORKSPACE"
            # libogg
            [ ! -d libogg-$ARCH ] && curl -fsSL https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.gz | tar -xz && mv libogg-1.3.5 libogg-$ARCH
            cd libogg-$ARCH
            ./configure --prefix="$PREFIX" $HOST_FLAG CFLAGS="$TARGET_FLAG" --enable-static --disable-shared
            make -j$(sysctl -n hw.logicalcpu) && make install
            cd ..
            
            # libvorbis
            [ ! -d libvorbis-$ARCH ] && curl -fsSL https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.gz | tar -xz && mv libvorbis-1.3.7 libvorbis-$ARCH
            cd libvorbis-$ARCH
            sed -i.bak 's/-force_cpusubtype_ALL//g' configure
            PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" ./configure --prefix="$PREFIX" $HOST_FLAG CFLAGS="$TARGET_FLAG" --with-ogg-includes="$PREFIX/include" --with-ogg-libraries="$PREFIX/lib" --enable-static --disable-shared
            make -j$(sysctl -n hw.logicalcpu) && make install
            cd ..
            
            # x264
            [ ! -d x264-$ARCH ] && git clone --depth=1 https://github.com/quatric/x264.git x264-$ARCH
            cd x264-$ARCH
            ./configure --prefix="$PREFIX" $HOST_FLAG --extra-cflags="$TARGET_FLAG" --extra-ldflags="$TARGET_FLAG" --enable-static --enable-pic --disable-cli --disable-opencl
            make -j$(sysctl -n hw.logicalcpu) && make install
            cd ..
          }

          build_arch "arm64" "" ""
          build_arch "x86_64" "-target x86_64-apple-macos11.0" "--host=x86_64-apple-darwin"

      - name: Build FFmpeg
        run: |
          DEPS="${GITHUB_WORKSPACE}/deps"
          
          build_ffmpeg() {
            local ARCH=$1
            local TARGET_FLAG=$2
            local PREFIX="$DEPS/$ARCH"
            local OUTDIR="$PWD/dist/$ARCH"
            
            mkdir -p build_ffmpeg_$ARCH
            cd build_ffmpeg_$ARCH
            
            export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
            ../../configure \\
              --prefix="$OUTDIR" \\
              --arch=$ARCH \\
              --enable-gpl \\
              --enable-libx264 \\
              --enable-libvorbis \\
              --disable-doc \\
              --disable-debug \\
              --pkg-config-flags="--static" \\
              --extra-cflags="-I$PREFIX/include $TARGET_FLAG" \\
              --extra-ldflags="-L$PREFIX/lib $TARGET_FLAG" || { cat ffbuild/config.log; exit 1; }
            
            make -j$(sysctl -n hw.logicalcpu)
            make install
            cd ..
          }
          
          build_ffmpeg "arm64" ""
          build_ffmpeg "x86_64" "-target x86_64-apple-macos11.0"

      - name: lipo into universal binaries
        run: |
          mkdir -p dist/universal
          for bin in ffmpeg ffprobe ffplay; do
            arm="dist/arm64/bin/$bin"
            x64="dist/x86_64/bin/$bin"
            if [[ -f "$arm" && -f "$x64" ]]; then
              chmod +x "$arm" "$x64"
              lipo -create -output "dist/universal/$bin" "$arm" "$x64"
              echo "Created universal $bin: $(lipo -info dist/universal/$bin)"
            elif [[ -f "$arm" ]]; then
              cp "$arm" "dist/universal/$bin"
            fi
          done

      - name: Package
        run: |
          cd dist/universal
          FILES="ffmpeg ffprobe"
          [ -f ffplay ] && FILES="$FILES ffplay"
          tar -czf "../../mobipeg-macos-universal.tar.gz" $FILES

      - uses: actions/upload-artifact@v4
        with:
          name: mobipeg-macos-universal
          path: mobipeg-macos-universal.tar.gz
          retention-days: 7

"""

content = content[:start_idx] + new_build_macos + content[end_idx:]

# 2. Update test-roundtrip matrix
content = content.replace('''          - target: macos-arm64
            os: macos-14
            ext: tar.gz
          - target: macos-x86_64
            os: macos-13
            ext: tar.gz''', '''          - target: macos-universal
            os: macos-14
            ext: tar.gz
          - target: macos-universal
            os: macos-13
            ext: tar.gz''')

# 3. Update build-gui matrix
content = content.replace('''          - target: macos-universal2
            os: macos-14
            ext: tar.gz''', '''          - target: macos-universal
            os: macos-14
            ext: tar.gz''')

# 4. Fix artifact download for gui
content = content.replace('''          name: mobipeg-${{ matrix.target == 'macos-universal2' && 'macos-universal' || matrix.target }}''', '''          name: mobipeg-${{ matrix.target }}''')
content = content.replace('''          elif [[ "${{ matrix.target }}" == "macos-universal2" ]]; then''', '''          elif [[ "${{ matrix.target }}" == "macos-universal" ]]; then''')

with open('.github/workflows/build.yml', 'w') as f:
    f.write(content)

