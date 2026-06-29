export PKG_CONFIG_PATH=/tmp/pkgconfig
mkdir -p /tmp/pkgconfig
cat << 'PC1' > /tmp/pkgconfig/ogg.pc
Name: ogg
Version: 1.3
Libs: -logg
PC1
cat << 'PC2' > /tmp/pkgconfig/vorbis.pc
Name: vorbis
Version: 1.3
Requires.private: ogg
Libs: -lvorbis
Libs.private: -lm
PC2
cat << 'PC3' > /tmp/pkgconfig/vorbisenc.pc
Name: vorbisenc
Version: 1.3
Requires.private: vorbis
Libs: -lvorbisenc
PC3
pkg-config --static --libs vorbisenc
