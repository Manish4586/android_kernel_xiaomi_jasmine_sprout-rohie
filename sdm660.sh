# Clang Build Script By Manish4586 <manish4586@github.com>

export KBUILD_BUILD_USER=Manish4586
export KBUILD_BUILD_HOST=rohi

echo -e "==============================================="	
echo    "         Rohie Kernel    -    catX         "	
echo -e "==============================================="	

LC_ALL=C date +%Y-%m-%d
date=`date +"%Y%m%d-%H%M"`
BUILD_START=$(date +"%s")
SOURCE=$PWD
HMM=$SOURCE/out
TOOL_DIR="/home/manish/toolchain/"
TOOL=$TOOL_DIR/proton-clang
PATH=$TOOL/bin:${PATH}
ANYKERN="/home/manish/Documents/kernel/"
ZIP=$ANYKERN/jasmine

make O=out ARCH=arm64 jasmine-perf_defconfig
make -j$(nproc --all) O=out \
                      ARCH=arm64 \
                      CC=clang \
                      AR=llvm-ar \
                      NM=llvm-nm \
                      OBJCOPY=llvm-objcopy \
                      OBJDUMP=llvm-objdump \
                      STRIP=llvm-strip \
                      CROSS_COMPILE_ARM32=arm-linux-gnueabi- \
                      CROSS_COMPILE=aarch64-linux-gnu- ;

cd $ZIP
cp $HMM/arch/arm64/boot/Image.gz-dtb $ZIP/
rm *.zip
FINAL_ZIP="RohieKernel-CatX-${date}.zip"
zip -r9 "${FINAL_ZIP}" *
rm Image.gz-dtb

cd $SOURCE

BUILD_END=$(date +"%s")
DIFF=$(($BUILD_END - $BUILD_START))
echo -e "Build completed in $(($DIFF / 60)) minute(s) and $(($DIFF % 60)) seconds."
echo -e "Done"
