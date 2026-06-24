# 让 Peach 自动重启目标程序功能在测试中可以忽略此文件

import argparse
import logging
import socket
import sys
from pathlib import Path

import pexpect


logger = logging.getLogger(__name__)

# coverage 启动流程用到的路径
repo_root = Path(__file__).resolve().parents[3]
build_script = repo_root / "Simulation" / "scripts" / "build.sh"
log_file = repo_root / "Log" / "control.log"

# guest 控制台登录信息与提示符
console_username = "root"
console_password = "root"
shell_prompt_pattern = r"root@.*#"

# 外部控制 socket 配置
control_host = "0.0.0.0"
default_control_port = 2111
control_accept_timeout = 10
control_recv_timeout = 5

# 协议名到 guest 服务名的映射
service_by_protocol = {
    "ssh": "ssh",
    "smtp": "exim4",
}

def configure_logging():
    # 同时输出日志到文件和终端
    log_file.parent.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        handlers=[
            logging.FileHandler(log_file, mode="w", encoding="utf-8"),
            logging.StreamHandler(),
        ],
        force=True,
    )


class QemuCoverageRuntime:
    # 封装 QEMU 生命周期与 guest 服务操作
    def __init__(self, protocol):
        self.protocol = protocol
        self.service_name = service_by_protocol[protocol]
        self.child = None

    def send_line_wait(self, text, expect_pattern=shell_prompt_pattern):
        child = self.child
        child.sendline(text)
        child.expect(expect_pattern)

    def start_qemu_service(self):
        logger.info("Starting QEMU in coverage mode via %s.", build_script)
        self.child = pexpect.spawn(
            "bash",
            [str(build_script), "coverage"],
            cwd=str(repo_root),
            encoding="utf-8",
            timeout=180,
        )

        logger.info("Waiting for login prompt.")
        self.child.expect("debian-mips login:")
        self.send_line_wait(console_username, "Password:")

        logger.info("Sending password.")
        self.send_line_wait(console_password, shell_prompt_pattern)

        logger.info("Configuring guest network.")
        self.send_line_wait("ifconfig eth0 192.168.153.2 netmask 255.255.255.0 up")
        self.send_line_wait("route add default gw 192.168.153.1 eth0")

        logger.info("Starting service `%s`.", self.service_name)
        self.send_line_wait(f"service {self.service_name} start")
        self.send_line_wait("echo -n > /var/mail/user")

    def restart_service(self):
        logger.info("Restarting service `%s`.", self.service_name)
        self.send_line_wait(f"service {self.service_name} restart")
        self.send_line_wait("echo -n > /var/mail/user")

    def shutdown_qemu(self):
        child = self.child
        if child is None:
            return

        try:
            logger.info("Entering QEMU monitor to quit VM.")
            child.sendline("")
            child.expect(shell_prompt_pattern, timeout=20)
            child.sendcontrol("a")
            child.send("c")
            child.expect(r"\(qemu\)", timeout=20)
            child.sendline("quit")
            child.expect(pexpect.EOF, timeout=20)
        except Exception:
            logger.warning("Graceful QEMU shutdown failed, trying force close.")
        finally:
            if child.isalive():
                try:
                    child.close(force=True)
                except Exception:
                    pass
            self.child = None

    def restart_qemu(self):
        logger.info("Restarting QEMU VM.")
        self.shutdown_qemu()
        self.start_qemu_service()


def parse_command_byte(data):
    # 仅支持原始字节 1/2/3
    value = data[0]
    if value in (1, 2, 3):
        return value
    return None


def send_result(client_sock, success):
    # 返回原始字节结果 1 表示成功 0 表示失败
    payload = bytes((1 if success else 0,))
    client_sock.sendall(payload)


def handle_command(runtime, command):
    # 1=关闭虚拟机 2=重启服务 3=重启虚拟机
    try:
        if command == 1:
            runtime.shutdown_qemu()
            return True
        if command == 2:
            runtime.restart_service()
            return True
        if command == 3:
            runtime.restart_qemu()
            return True
        return False
    except Exception as exc:
        logger.error("Control command %s failed: %s", command, exc)
        return False


def run_control_server(runtime, control_port):
    # 启动完成后开放控制端口并等待控制连接
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server_sock:
        server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_sock.bind((control_host, control_port))
        server_sock.listen()
        server_sock.settimeout(control_accept_timeout)
        logger.info("Control server is listening on %s:%s.", control_host, control_port)

        try:
            client_sock, client_addr = server_sock.accept()
        except socket.timeout:
            logger.error("No control client connected within %s seconds, exiting.", control_accept_timeout)
            return False
        except OSError as exc:
            logger.error("Control server accept failed: %s", exc)
            return False

        logger.info("Accepted control connection from %s:%s.", client_addr[0], client_addr[1])
        with client_sock:
            client_sock.settimeout(control_recv_timeout)
            while True:
                try:
                    data = client_sock.recv(1)
                except socket.timeout:
                    continue
                except OSError as exc:
                    logger.error("Control socket recv failed: %s", exc)
                    break

                if not data:
                    logger.info("Control client disconnected.")
                    break

                command = parse_command_byte(data)
                if command is None:
                    logger.warning("Unsupported control command byte: %s.", data[0])
                    try:
                        send_result(client_sock, False)
                    except OSError as exc:
                        logger.error("Failed to send command result to client: %s", exc)
                        break
                    continue

                logger.info("Received control command: %s.", command)
                success = handle_command(runtime, command)
                logger.info("Control command %s result: %s.", command, success)
                try:
                    send_result(client_sock, success)
                except OSError as exc:
                    logger.error("Failed to send command result to client: %s", exc)
                    break

        return True

def parse_args():
    parser = argparse.ArgumentParser(description="QEMU coverage control server")
    parser.add_argument(
        "protocol",
        nargs="?",
        choices=sorted(service_by_protocol.keys()),
        default="ssh",
        help="Protocol to control (ssh or smtp).",
    )
    parser.add_argument(
        "--control-port",
        dest="control_port",
        type=int,
        default=default_control_port,
        help="Control TCP port, default is 2111.",
    )
    args = parser.parse_args()
    if args.control_port < 1 or args.control_port > 65535:
        parser.error("--control-port must be between 1 and 65535")
    return args


def run_control_entry():
    args = parse_args()
    configure_logging()

    runtime = QemuCoverageRuntime(args.protocol)
    # 先按协议启动服务 再开放 2111 端口
    server_ok = False
    try:
        runtime.start_qemu_service()
        server_ok = run_control_server(runtime, args.control_port)
    except Exception as exc:
        logger.error("Control server exited with error: %s", exc)
    finally:
        try:
            runtime.shutdown_qemu()
        except Exception as exc:
            logger.error("Failed to shutdown qemu on exit: %s", exc)

    if not server_ok:
        logger.error("Control workflow failed for protocol `%s`.", args.protocol)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(run_control_entry())
