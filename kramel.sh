#!/usr/bin/env bash
# Written by: cyberknight777
# YAKB v2.0
# Copyright (c) 2022-2023 Cyber Knight <cyberknight755@gmail.com>
#
#			GNU GENERAL PUBLIC LICENSE
#			 Version 3, 29 June 2007
#
# Copyright (C) 2007 Free Software Foundation, Inc. <https://fsf.org/>
# Everyone is permitted to copy and distribute verbatim copies
# of this license document, but changing it is not allowed.

# Some Placeholders: [!] [*] [✓] [✗]

# Default defconfig to use for builds.
export CONFIG=neternels_defconfig

# Default directory where kernel is located in.
KDIR=$(pwd)
export KDIR

# Default linker to use for builds.
export LINKER="ld"

# Device name.
export DEVICE="Redmi Note 10"

# Date of build.
DATE=$(date +"%Y-%m-%d")
export DATE

# Device codename.
export CODENAME="sunny"

# Builder name.
export BUILDER="cyberknight777"

# Kernel repository URL.
export REPO_URL="https://github.com/neternels/android_kernel_xiaomi_sunny"

# Commit hash of HEAD.
COMMIT_HASH=$(git rev-parse --short HEAD)
export COMMIT_HASH

# Build status. Set 1 for release builds. | Set 0 for bleeding edge builds.
if [ "${RELEASE}" == 1 ]; then
	export STATUS="Release"
	export CHATID=-1001301508914
	export re="rc"
else
	export STATUS="Bleeding-Edge"
	export CHATID=-1001522330051
	export re="r"
fi

# Telegram Information. Set 1 to enable. | Set 0 to disable.
export TGI=1

# Number of jobs to run.
PROCS=$(nproc --all)
export PROCS

# Compiler to use for builds.
export COMPILER=gcc

# Module building support. Set 1 to enable. | Set 0 to disable.
export MODULE=1

# Requirements
if [ "${CI}" == 0 ]; then
	if ! hash dialog make curl wget unzip find 2>/dev/null; then
		echo -e "\n\e[1;31m[✗] Install dialog, make, curl, wget, unzip, and find! \e[0m"
		exit 1
	fi
fi

if [[ ${COMPILER} == gcc ]]; then
	if [ ! -d "${KDIR}/gcc64" ]; then
		curl -sL https://github.com/mvaisakh/gcc-arm64/archive/refs/heads/gcc-master.tar.gz | tar -xzf -
		mv "${KDIR}"/gcc-arm64-gcc-master "${KDIR}"/gcc64
	fi

	if [ ! -d "${KDIR}/gcc32" ]; then
		curl -sL https://github.com/mvaisakh/gcc-arm/archive/refs/heads/gcc-master.tar.gz | tar -xzf -
		mv "${KDIR}"/gcc-arm-gcc-master "${KDIR}"/gcc32
	fi

	KBUILD_COMPILER_STRING=$("${KDIR}"/gcc64/bin/aarch64-elf-gcc --version | head -n 1)
	export KBUILD_COMPILER_STRING
	export PATH="${KDIR}"/gcc32/bin:"${KDIR}"/gcc64/bin:/usr/bin/:${PATH}
	MAKE+=(
		O=out
		CROSS_COMPILE=aarch64-elf-
		CROSS_COMPILE_ARM32=arm-eabi-
		LD="${KDIR}"/gcc64/bin/aarch64-elf-"${LINKER}"
		AR=aarch64-elf-ar
		AS=aarch64-elf-as
		NM=aarch64-elf-nm
		OBJDUMP=aarch64-elf-objdump
		OBJCOPY=aarch64-elf-objcopy
		CC=aarch64-elf-gcc
	)

elif [[ ${COMPILER} == clang ]]; then
	if [ ! -f "${KDIR}/neutron-clang/bin/clang" ]; then
		rm -rf "${KDIR}"/neutron-clang
		mkdir "${KDIR}"/neutron-clang
		cd "${KDIR}"/neutron-clang || exit 1
		bash <(curl -s "https://raw.githubusercontent.com/Neutron-Toolchains/antman/main/antman") -S
		cd "${KDIR}" || exit 1
	fi

	KBUILD_COMPILER_STRING=$("${KDIR}"/neutron-clang/bin/clang -v 2>&1 | head -n 1 | sed 's/(https..*//' | sed 's/ version//')
	export KBUILD_COMPILER_STRING
	export PATH=$KDIR/proton-clang/bin/:/usr/bin/:${PATH}
	MAKE+=(
		O=out
		LLVM=1
	)
fi

if [[ ${MODULE} == 1 ]]; then
	if [ ! -d "${KDIR}"/modules ]; then
		git clone --depth=1 https://github.com/neternels/neternels-modules "${KDIR}"/modules
	fi
fi

if [ ! -d "${KDIR}/anykernel3-sunny/" ]; then
	git clone --depth=1 https://github.com/neternels/anykernel3 -b sunny anykernel3-sunny
fi

if [ ! -f "${KDIR}/version" ]; then
	echo -e "\n\e[1;31m[✗] version file not found!!! Read https://github.com/cyberknight777/YAKB#version-file for more information.\e[0m"
	exit 1
fi

KBUILD_BUILD_VERSION=$(grep num= version | cut -d= -f2)
export KBUILD_BUILD_VERSION
export KBUILD_BUILD_HOST="builder"
export KBUILD_BUILD_USER="cyberknight777"
VERSION=$(grep ver= version | cut -d= -f2)
kver="${KBUILD_BUILD_VERSION}"
zipn=NetErnels-sunny-"${VERSION}"

if [[ ${MODULE} == "1" ]]; then
	modn="${zipn}-modules"
fi

# A function to exit on SIGINT.
exit_on_signal_SIGINT() {
	echo -e "\n\n\e[1;31m[✗] Received INTR call - Exiting...\e[0m"
	exit 0
}
trap exit_on_signal_SIGINT SIGINT

# A function to send message(s) via Telegram's BOT api.
tg() {
	curl -sX POST https://api.telegram.org/bot"${TOKEN}"/sendMessage \
		-d chat_id="${CHATID}" \
		-d parse_mode=Markdown \
		-d disable_web_page_preview=true \
		-d text="$1" &>/dev/null
}

# A function to send file(s) via Telegram's BOT api.
tgs() {
	MD5=$(md5sum "$1" | cut -d' ' -f1)
	curl -fsSL -X POST -F document=@"$1" https://api.telegram.org/bot"${TOKEN}"/sendDocument \
		-F "chat_id=${CHATID}" \
		-F "parse_mode=Markdown" \
		-F "caption=$2 | *MD5*: \`$MD5\`"
}

# A function to clean kernel source prior building.
clean() {
	echo -e "\n\e[1;93m[*] Cleaning source and out/ directory! \e[0m"
	make clean && make mrproper && rm -rf "${KDIR}"/out
	echo -e "\n\e[1;32m[✓] Source cleaned and out/ removed! \e[0m"
}

# A function to regenerate defconfig.
rgn() {
	echo -e "\n\e[1;93m[*] Regenerating defconfig! \e[0m"
	make "${MAKE[@]}" $CONFIG
	cp -rf "${KDIR}"/out/.config "${KDIR}"/arch/arm64/configs/$CONFIG
	echo -e "\n\e[1;32m[✓] Defconfig regenerated! \e[0m"
}

# A function to open a menu based program to update current config.
mcfg() {
	rgn
	echo -e "\n\e[1;93m[*] Making Menuconfig! \e[0m"
	make "${MAKE[@]}" menuconfig
	cp -rf "${KDIR}"/out/.config "${KDIR}"/arch/arm64/configs/$CONFIG
	echo -e "\n\e[1;32m[✓] Saved Modifications! \e[0m"
}

# A function to build the kernel.
img() {
	if [[ ${TGI} == "1" ]]; then
		if [[ ${RELEASE} != "1" ]]; then
			wget https://raw.githubusercontent.com/cyberknight777/my-stuffs/main/img/net.jpg
			curl -F photo=@net.jpg https://api.telegram.org/bot"${TOKEN}"/sendPhoto -F chat_id=-1001522330051
			rm net.jpg
		fi
		tg "
*Build Number*: \`${kver}\`
*Status*: \`${STATUS}\`
*Builder*: \`${BUILDER}\`
*Core count*: \`$(nproc --all)\`
*Device*: \`${DEVICE} [${CODENAME}]\`
*Kernel Version*: \`$(make kernelversion 2>/dev/null)\`
*Date*: \`$(date)\`
*Zip Name*: \`${zipn}\`
*Compiler*: \`${KBUILD_COMPILER_STRING}\`
*Linker*: \`$("${KDIR}"/gcc64/bin/aarch64-elf-${LINKER} -v | head -n1 | sed 's/(compatible with [^)]*)//' |
			head -n 1 | perl -pe 's/\(http.*?\)//gs' | sed -e 's/  */ /g' -e 's/[[:space:]]*$//')\`
*Branch*: \`$(git rev-parse --abbrev-ref HEAD)\`
*Last Commit*: [${COMMIT_HASH}](${REPO_URL}/commit/${COMMIT_HASH})
"
	fi
	rgn
	echo -e "\n\e[1;93m[*] Building Kernel! \e[0m"
	BUILD_START=$(date +"%s")
	time make -j"$PROCS" "${MAKE[@]}" Image dtbo.img 2>&1 | tee log.txt
	BUILD_END=$(date +"%s")
	DIFF=$((BUILD_END - BUILD_START))
	if [ -f "${KDIR}/out/arch/arm64/boot/Image" ]; then
		if [[ ${TGI} == "1" ]]; then
			tg "*Kernel Built after $((DIFF / 60)) minute(s) and $((DIFF % 60)) second(s)*"
		fi
		echo -e "\n\e[1;32m[✓] Kernel built after $((DIFF / 60)) minute(s) and $((DIFF % 60)) second(s)! \e[0m"
	else
		if [[ ${TGI} == "1" ]]; then
			tgs "log.txt" "*Build failed*"
		fi
		echo -e "\n\e[1;31m[✗] Build Failed! \e[0m"
		exit 1
	fi
}

# A function to build DTBs.
dtb() {
	rgn
	echo -e "\n\e[1;93m[*] Building DTBS! \e[0m"
	time make -j"$PROCS" "${MAKE[@]}" dtbs dtbo.img
	echo -e "\n\e[1;32m[✓] Built DTBS! \e[0m"
}

# A function to build out-of-tree modules.
mod() {
	if [[ ${TGI} == "1" ]]; then
		tg "*Building Modules!*"
	fi
	rgn
	echo -e "\n\e[1;93m[*] Building Modules! \e[0m"
	mkdir -p "${KDIR}"/out/modules
	make "${MAKE[@]}" modules_prepare
	make -j"$PROCS" "${MAKE[@]}" modules INSTALL_MOD_PATH="${KDIR}"/out/modules
	make "${MAKE[@]}" modules_install INSTALL_MOD_PATH="${KDIR}"/out/modules
	find "${KDIR}"/out/modules -type f -iname '*.ko' -exec cp {} "${KDIR}"/modules/system/lib/modules/ \;
	cd "${KDIR}"/modules || exit 1
	zip -r9 "${modn}".zip . -x ".git*" -x "README.md" -x "LICENSE" -x "*.zip"
	cd ../
	echo -e "\n\e[1;32m[✓] Built Modules! \e[0m"
}

# A function to build an AnyKernel3 zip.
mkzip() {
	if [[ ${TGI} == "1" ]]; then
		tg "*Building zip!*"
	fi
	echo -e "\n\e[1;93m[*] Building zip! \e[0m"
	mkdir -p "${KDIR}"/anykernel3-sunny/dtbs
	mv "${KDIR}"/out/arch/arm64/boot/dtbo.img "${KDIR}"/anykernel3-sunny/dtbs
	cat "${KDIR}"/out/arch/arm64/boot/dts/qcom/sm6150.dtb >"${KDIR}"/anykernel3-sunny/dtb
	sed -i 's/2047/4095/g' "${KDIR}"/arch/arm64/boot/dts/qcom/dsi_panel_k7_38_0c_0a_fhdp_video.dtsi
	time make -j"$PROCS" "${MAKE[@]}" dtbo.img
	mv "${KDIR}"/out/arch/arm64/boot/dtbo.img "${KDIR}"/anykernel3-sunny/dtbs/dtbo_aospa.img
	mv "${KDIR}"/out/arch/arm64/boot/Image "${KDIR}"/anykernel3-sunny
	cd "${KDIR}"/anykernel3-sunny || exit 1
	zip -r9 "$zipn".zip . -x ".git*" -x "README.md" -x "LICENSE" -x "*.zip"
	echo -e "\n\e[1;32m[✓] Built zip! \e[0m"
	if [[ ${OTA} == "1" ]]; then
		git clone https://github.com/NetErnels/devices.git
		cd devices || exit 1
		echo "https://cyberknight777:$PASSWORD@github.com" >.pwd
		git config credential.helper "store --file .pwd"
		sha1=$(sha1sum ../"${zipn}".zip | cut -d ' ' -f1)
		if [[ ${RELEASE} != "1" ]]; then
			rm sunny/changelog_r.md
			wget "${CL_LINK}/raw" -O sunny/changelog_r.md
			echo "
{
  \"kernel\": {
  \"name\": \"NetErnels\",
  \"version\": \"$VERSION\",
  \"link\": \"https://github.com/NetErnels/devices/releases/download/$VERSION/$zipn.zip\",
  \"changelog_url\": \"https://raw.githubusercontent.com/NetErnels/devices/master/sunny/changelog_r.md\",
  \"date\": \"$DATE\",
  \"sha1\": \"$sha1\"
  },
  \"support\": {
    \"link\": \"https://t.me/neternels_chat\"
  }
}
" >sunny/NetErnels-r.json
			git add sunny/NetErnels-r.json sunny/changelog_r.md || exit 1
			git commit -s -m "NetErnels: Update $CODENAME to $VERSION release" -m "- This is a bleeding edge release."
			git commit --amend --reset-author --no-edit
			git push
			gh release create "${VERSION}" -t "NetErnels for $CODENAME [BLEEDING EDGE] - $VERSION"
			gh release upload "${VERSION}" ../"${zipn}.zip"
			gh release upload "${VERSION}" ../../modules/"${modn}.zip"
		else
			rm sunny/changelog.md
			wget "${CL_LINK}"/raw -O sunny/changelog.md
			echo "
{
  \"kernel\": {
  \"name\": \"NetErnels\",
  \"version\": \"$VERSION\",
  \"link\": \"https://github.com/NetErnels/devices/releases/download/$VERSION/$zipn.zip\",
  \"changelog_url\": \"https://raw.githubusercontent.com/NetErnels/devices/master/sunny/changelog.md\",
  \"date\": \"$DATE\",
  \"sha1\": \"$sha1\"
  },
  \"support\": {
    \"link\": \"https://t.me/neternels_chat\"
  }
}
" >sunny/NetErnels-v.json
			git add sunny/NetErnels-v.json sunny/changelog.md || exit 1
			git commit -s -m "NetErnels: Update $CODENAME to $VERSION release" -m "- This is a stable release."
			git commit --amend --reset-author --no-edit
			git push
			gh release create "${VERSION}" -t "NetErnels for $CODENAME [RELEASE] - $VERSION"
			gh release upload "${VERSION}" ../"${zipn}.zip"
			gh release upload "${VERSION}" ../../modules/"${modn}.zip"
		fi
		cd ../ || exit 1
		rm -rf devices || exit 1
	fi
	if [[ ${TGI} == "1" ]]; then
		tgs "${zipn}.zip" "*#${kver} ${KBUILD_COMPILER_STRING}*"
		if [[ ${MODULE} == "1" ]]; then
			cd ../modules || exit 1
			tgs "${modn}.zip" "*#${kver} ${KBUILD_COMPILER_STRING}*"
		fi
		tg "
*Build*: https://github.com/NetErnels/devices/releases/download/$VERSION/$zipn.zip
*Changelog*: https://github.com/NetErnels/devices/blob/master/sunny/changelog\_${re}.md
*OTA*: https://raw.githubusercontent.com/NetErnels/devices/master/sunny/NetErnels-${re}.json
"
	fi
}

# A function to build specific objects.
obj() {
	rgn
	echo -e "\n\e[1;93m[*] Building ${1}! \e[0m"
	time make -j"$PROCS" "${MAKE[@]}" "$1"
	echo -e "\n\e[1;32m[✓] Built ${1}! \e[0m"
}

# A function to uprev localversion in defconfig.
upr() {
	echo -e "\n\e[1;93m[*] Bumping localversion to -NetErnels-${1}! \e[0m"
	"${KDIR}"/scripts/config --file "${KDIR}"/arch/arm64/configs/$CONFIG --set-str CONFIG_LOCALVERSION "-NetErnels-${1}"
	rgn
	if [ "${CI}" == "0" ]; then
		git add arch/arm64/configs/$CONFIG
		git commit -S -s -m "neternels_defconfig: Bump to \`${1}\`"
	fi
	echo -e "\n\e[1;32m[✓] Bumped localversion to -NetErnels-${1}! \e[0m"
}

# A function to showcase the options provided for args-based usage.
helpmenu() {
	echo -e "\n\e[1m
usage: bash $0 <arg>

example: bash $0 mcfg
example: bash $0 mcfg img
example: bash $0 mcfg img mkzip
example: bash $0 --obj=drivers/android/binder.o
example: bash $0 --obj=kernel/sched/
example: bash $0 --upr=r16

	 mcfg   Runs make menuconfig
	 img    Builds Kernel
	 dtb    Builds dtb(o).img
	 mod    Builds out-of-tree modules
	 mkzip  Builds anykernel3 zip
	 --obj  Builds specific driver/subsystem
	 rgn    Regenerates defconfig
	 --upr  Uprevs kernel version in defconfig
\e[0m"
}

# A function to setup menu-based usage.
ndialog() {
	HEIGHT=16
	WIDTH=40
	CHOICE_HEIGHT=30
	BACKTITLE="Yet Another Kernel Builder"
	TITLE="YAKB v1.0"
	MENU="Choose one of the following options: "
	OPTIONS=(1 "Build kernel"
		2 "Build DTBs"
		3 "Build modules"
		4 "Open menuconfig"
		5 "Regenerate defconfig"
		6 "Uprev localversion"
		7 "Build AnyKernel3 zip"
		8 "Build a specific object"
		9 "Clean"
		10 "Exit"
	)
	CHOICE=$(dialog --clear \
		--backtitle "$BACKTITLE" \
		--title "$TITLE" \
		--menu "$MENU" \
		$HEIGHT $WIDTH $CHOICE_HEIGHT \
		"${OPTIONS[@]}" \
		2>&1 >/dev/tty)
	clear
	case "$CHOICE" in
	1)
		clear
		img
		echo -ne "\e[1mPress enter to continue or 0 to exit! \e[0m"
		read -r a1
		if [ "$a1" == "0" ]; then
			exit 0
		else
			clear
			ndialog
		fi
		;;
	2)
		clear
		dtb
		echo -ne "\e[1mPress enter to continue or 0 to exit! \e[0m"
		read -r a1
		if [ "$a1" == "0" ]; then
			exit 0
		else
			clear
			ndialog
		fi
		;;
	3)
		clear
		mod
		echo -ne "\e[1mPress enter to continue or 0 to exit! \e[0m"
		read -r a1
		if [ "$a1" == "0" ]; then
			exit 0
		else
			clear
			ndialog
		fi
		;;
	4)
		clear
		mcfg
		echo -ne "\e[1mPress enter to continue or 0 to exit! \e[0m"
		read -r a1
		if [ "$a1" == "0" ]; then
			exit 0
		else
			clear
			ndialog
		fi
		;;
	5)
		clear
		rgn
		echo -ne "\e[1mPress enter to continue or 0 to exit! \e[0m"
		read -r a1
		if [ "$a1" == "0" ]; then
			exit 0
		else
			clear
			ndialog
		fi
		;;
	6)
		dialog --inputbox --stdout "Enter version number: " 15 50 | tee .t
		ver=$(cat .t)
		clear
		upr "$ver"
		rm .t
		echo -ne "\e[1mPress enter to continue or 0 to exit! \e[0m"
		read -r a1
		if [ "$a1" == "0" ]; then
			exit 0
		else
			clear
			ndialog
		fi
		;;
	7)
		mkzip
		echo -ne "\e[1mPress enter to continue or 0 to exit! \e[0m"
		read -r a1
		if [ "$a1" == "0" ]; then
			exit 0
		else
			clear
			ndialog
		fi
		;;
	8)
		dialog --inputbox --stdout "Enter object path: " 15 50 | tee .f
		ob=$(cat .f)
		if [ -z "$ob" ]; then
			dialog --inputbox --stdout "Enter object path: " 15 50 | tee .f
		fi
		clear
		obj "$ob"
		rm .f
		echo -ne "\e[1mPress enter to continue or 0 to exit! \e[0m"
		read -r a1
		if [ "$a1" == "0" ]; then
			exit 0
		else
			clear
			ndialog
		fi
		;;
	9)
		clear
		clean
		img
		echo -ne "\e[1mPress enter to continue or 0 to exit! \e[0m"
		read -r a1
		if [ "$a1" == "0" ]; then
			exit 0
		else
			clear
			ndialog
		fi
		;;
	10)
		echo -e "\n\e[1m Exiting YAKB...\e[0m"
		sleep 3
		exit 0
		;;
	esac
}

if [ "${CI}" == 1 ]; then
	upr "${VERSION}"
fi

if [[ -z $* ]]; then
	ndialog
fi

for arg in "$@"; do
	case "${arg}" in
	"mcfg")
		mcfg
		;;
	"img")
		img
		;;
	"dtb")
		dtb
		;;
	"mod")
		mod
		;;
	"mkzip")
		mkzip
		;;
	"--obj="*)
		object="${arg#*=}"
		if [[ -z $object ]]; then
			echo "Use --obj=filename.o"
			exit 1
		else
			obj "$object"
		fi
		;;
	"rgn")
		rgn
		;;
	"--upr="*)
		vers="${arg#*=}"
		if [[ -z $vers ]]; then
			echo "Use --upr=version"
			exit 1
		else
			upr "$vers"
		fi
		;;
	"clean")
		clean
		;;
	"help")
		helpmenu
		exit 1
		;;
	*)
		helpmenu
		exit 1
		;;
	esac
done
