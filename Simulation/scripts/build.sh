QEMU_BUILD_DIR=Qemu/build
QEMU_HOME=$QEMU_BUILD_DIR/install/bin
TCG_PLUGINS_HOME=$QEMU_BUILD_DIR/contrib/plugins
MIRRORS_DIR=Simulation/mirrors
SCRIPT_DIR=Simulation/scripts

set -e

MODE=${1:-none}
COVERAGE_PORT=${2:-3111}

validate_port() {
    case "$1" in
        ''|*[!0-9]*)
            return 1
            ;;
        *)
            if [ "$1" -lt 1 ] || [ "$1" -gt 65535 ]; then
                return 1
            fi
            ;;
    esac
    return 0
}

make -C $TCG_PLUGINS_HOME -j$(nproc)

if ip link show Virbr0 >/dev/null 2>&1; then
    $SCRIPT_DIR/unset_bridge.sh
fi
$SCRIPT_DIR/set_bridge.sh

case "$MODE" in
    coverage)
        if ! validate_port "$COVERAGE_PORT"; then
            printf 'Invalid coverage port: %s\n' "$COVERAGE_PORT"
            printf 'Usage: %s [coverage [port]|none]\n' "$0"
            exit 1
        fi
        PLUGIN_ARG="-plugin $TCG_PLUGINS_HOME/libcoverage.so,port=$COVERAGE_PORT"
        ;;
    none)
        PLUGIN_ARG=""
        ;;
    *)
        printf 'Usage: %s [coverage [port]|none]\n' "$0"
        exit 1
        ;;
esac

$QEMU_HOME/qemu-system-mips \
    -M malta \
    -kernel $MIRRORS_DIR/vmlinux-2.6.32-5-4kc-malta \
    -hda $MIRRORS_DIR/debian_squeeze_mips_standard.qcow2 \
    -append "root=/dev/sda1 console=tty0" \
    -netdev tap,id=tapnet,ifname=tap0,script=no,downscript=no \
    -device rtl8139,netdev=tapnet \
    -d plugin \
    $PLUGIN_ARG \
    -nographic
