#!/usr/bin/env bash

# 执行命令并在失败时立即退出。
# 该脚本需要连续构建 Buildroot、Linux 和 simplefs，任何一步失败都不应继续执行后续
# 步骤，否则可能复制到不完整的镜像文件。
function ASSERT
{
    $*
    RES=$?
    if [ $RES -ne 0 ]; then
        echo '断言失败："' $* '"'
        exit $RES
    fi
}

# 成功提示使用绿色输出，便于在长构建日志中定位阶段边界。
PASS_COLOR='\e[32;01m'
NO_COLOR='\e[0m'
function OK
{
    printf " [ ${PASS_COLOR} OK ${NO_COLOR} ]\n"
}

# 外部源码统一放在 /tmp 下，和 mk/external.mk 中下载的 Buildroot/Linux/simplefs
# 目录约定保持一致。
SRC_DIR=/tmp

# 并行构建参数；默认使用宿主所有 CPU 核心。
PARALLEL="-j$(nproc)"

# 最终产物输出目录：rootfs.cpio、Linux Image 和 simplefs.ko 都会复制到这里。
OUTPUT_DIR=./build/linux-image/
mkdir -p $OUTPUT_DIR

# 自定义 Buildroot 包来源。每个 C 文件会被包装成一个 Buildroot 本地包，
# 用于系统模式下测试 RTC 等设备行为。
BR_PKG_RTC_DIR=./tests/system/br_pkgs/rtc

function create_br_pkg_config()
{
    local pkg_name=$1
    local output_path=$2

    # 为自定义包生成 Buildroot Kconfig 片段，使该包可以被 buildroot.config 启用。
    cat << EOF > "${output_path}"
config BR2_PACKAGE_${pkg_name^^}
    bool "${pkg_name}"
    help
        ${pkg_name} 测试包。
EOF
}

function create_br_pkg_makefile()
{
    local pkg_name=$1
    local output_path=$2

    # 为自定义包生成 Buildroot .mk 文件，声明源码位置、构建命令和安装路径。
    cat << EOF > "${output_path}"
################################################################################
#
# ${pkg_name} 测试包
#
################################################################################

${pkg_name^^}_VERSION = 1.0
${pkg_name^^}_SITE = package/${pkg_name}/src
${pkg_name^^}_SITE_METHOD = local

define ${pkg_name^^}_BUILD_CMDS
	\$(MAKE) CC="\$(TARGET_CC)" LD="\$(TARGET_LD)" -C \$(@D)
endef

define ${pkg_name^^}_INSTALL_TARGET_CMDS
	\$(INSTALL) -D -m 0755 \$(@D)/${pkg_name} \$(TARGET_DIR)/usr/bin
endef

\$(eval \$(generic-package))
EOF
}

function create_br_pkg_src()
{
    local pkg_name=$1
    local src_c_file=$2
    local output_path=$3
    local mk_output_path=$3/Makefile
    local src_output_path=$3/$pkg_name.c

    # 创建 Buildroot 本地包的源码目录。
    mkdir -p ${output_path}

    # 生成最小 Makefile；输出二进制名称保持小写，和包名一致。
    cat << EOF > "${mk_output_path}"
all:
	\$(CC) ${pkg_name}.c -o ${pkg_name}
EOF

    # 复制测试 C 源码到 Buildroot 包源码目录。
    cp -f ${src_c_file} ${src_output_path}
}

function update_br_pkg_config()
{
    local pkg_name=$1
    local br_pkg_config_file="${SRC_DIR}/buildroot/package/Config.in"
    local source_line="    source \"package/${pkg_name}/Config.in\""

    # 若菜单中尚未包含该包，则把它追加到 Custom packages 菜单下。
    if ! grep -q "${pkg_name}" "${br_pkg_config_file}"; then
        sed -i '/^menu "Custom packages"/,/^endmenu$/{
            /^endmenu$/i\
'"${source_line}"'
        }' "${br_pkg_config_file}"
    fi
}

# 从零构建 rootfs.cpio 时，向 Buildroot 注入项目自带的自定义测试包。
function do_patch_buildroot
{
    local br_pkg_config_file="${SRC_DIR}/buildroot/package/Config.in"

    # 若 Buildroot 包菜单中还没有 Custom packages 块，则先创建该菜单。
    if ! grep -q "Custom packages" "${br_pkg_config_file}"; then
        cat << EOF >> "${br_pkg_config_file}"
menu "Custom packages"
endmenu
EOF
    fi

    # 把 tests/system/br_pkgs/rtc 下的每个 C 文件都包装成一个 Buildroot 包。
    for c in $(find ${BR_PKG_RTC_DIR} -type f); do
        local basename="$(basename ${c})"
        local pkg_name="${basename%.*}"

        mkdir -p ${SRC_DIR}/buildroot/package/${pkg_name}

        create_br_pkg_config ${pkg_name} ${SRC_DIR}/buildroot/package/${pkg_name}/Config.in
        create_br_pkg_makefile ${pkg_name} ${SRC_DIR}/buildroot/package/${pkg_name}/${pkg_name}.mk
        create_br_pkg_src ${pkg_name} ${c} ${SRC_DIR}/buildroot/package/${pkg_name}/src

        update_br_pkg_config ${pkg_name}
    done
}

function do_buildroot
{
    # 使用项目提供的 Buildroot/BusyBox 配置，保证 rootfs 内容可复现。
    cp -f assets/system/configs/buildroot.config ${SRC_DIR}/buildroot/.config
    cp -f assets/system/configs/busybox.config ${SRC_DIR}/buildroot/busybox.config
    # Buildroot 不允许 LD_LIBRARY_PATH 中包含当前工作目录；部分用户环境会设置该值，
    # 因此构建前显式清空，避免 Buildroot 在配置阶段报错。
    unset LD_LIBRARY_PATH
    do_patch_buildroot
    pushd ${SRC_DIR}/buildroot
    ASSERT make olddefconfig
    ASSERT make ${PARALLEL}
    popd
    cp -f ${SRC_DIR}/buildroot/output/images/rootfs.cpio ${OUTPUT_DIR}
}

function do_linux
{
    # Linux 使用项目内置配置，并复用 Buildroot 生成的 riscv32 交叉工具链。
    cp -f assets/system/configs/linux.config ${SRC_DIR}/linux/.config
    export PATH="${SRC_DIR}/buildroot/output/host/bin:${PATH}"
    export CROSS_COMPILE=riscv32-buildroot-linux-gnu-
    export ARCH=riscv
    pushd ${SRC_DIR}/linux
    ASSERT make olddefconfig
    ASSERT make ${PARALLEL}
    popd
    cp -f ${SRC_DIR}/linux/arch/riscv/boot/Image ${OUTPUT_DIR}
}

function do_simplefs
{
    # simplefs 是额外构建的内核模块，需要使用刚刚构建出的 Linux 内核目录。
    pushd $SRC_DIR/simplefs
    ASSERT make KDIR=$SRC_DIR/linux $PARALLEL
    popd
    cp -f $SRC_DIR/simplefs/simplefs.ko $OUTPUT_DIR
}

# 三个阶段按依赖顺序执行：rootfs 先提供交叉工具链，Linux 使用该工具链，simplefs
# 再依赖 Linux 构建目录。
do_buildroot && OK
do_linux && OK
do_simplefs && OK
