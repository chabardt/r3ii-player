#!/bin/sh
set -e
ROOTFS="$1"; KERNEL="$2"; OUT="$3"; VER=0; CHUNK=520997
WORK=$(mktemp -d); ISO="$WORK/iso"; OV="$ISO/ota_v$VER"
mkdir -p "$OV"
printf "current_version=%s\n" "$VER" > "$ISO/ota_config.in"
: > "$OV/ota_v$VER.ok"
chunk_img(){ name="$1"; file="$2"
  overall=$(md5sum "$file"|awk '{print $1}'); size=$(stat -c%s "$file")
  cd=$(mktemp -d); split -b $CHUNK "$file" "$cd/c."
  prev="$overall"; i=0; : > "$OV/ota_md5_$name.$overall"
  for f in $(ls "$cd"/c.*|sort); do
    num=$(printf "%04d" $i)
    cp "$f" "$OV/$name.$num.$prev"
    m=$(md5sum "$f"|awk '{print $1}')
    echo "$m" >> "$OV/ota_md5_$name.$overall"
    prev="$m"; i=$((i+1))
  done
  rm -rf "$cd"
  echo "$overall $size"
}
set -- $(chunk_img xImage "$KERNEL"); KMD5=$1; KSIZE=$2
set -- $(chunk_img rootfs.squashfs "$ROOTFS"); RMD5=$1; RSIZE=$2
cat > "$OV/ota_update.in" <<EOT
ota_version=$VER

img_type=kernel
img_name=xImage
img_size=$KSIZE
img_md5=$KMD5

img_type=rootfs
img_name=rootfs.squashfs
img_size=$RSIZE
img_md5=$RMD5
EOT
xorriso -as mkisofs -V CDROM -J -r -o "$OUT" "$ISO/" 2>/dev/null
echo "built $(basename "$OUT"): $(stat -c%s "$OUT") bytes ; rootfs md5=$RMD5 size=$RSIZE ; kernel size=$KSIZE"
rm -rf "$WORK"
