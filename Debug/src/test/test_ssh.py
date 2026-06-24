# SSH 连接测试脚本，支持 none/init/random 三种模式用于覆盖率采集

import logging
import random
import paramiko

logger = logging.getLogger(__name__)

ssh_commands = [
    "ls",
    "ifconfig",
    "ping -c 1 127.0.0.1",
    "ip link",
    "hostname",
    "whoami",
    "netstat -antp",
    "echo 123",
]


def connect_ssh(host, port, username, password):
    # 建立 SSH 连接
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    logger.info("Connecting to %s@%s:%s.", username, host, port)
    client.connect(
        hostname=host,
        port=port,
        username=username,
        password=password,
        look_for_keys=False,
        timeout=10,
    )
    return client


def pick_ssh_command(mode):
    # 根据模式决定是否执行命令
    if mode == "none":
        return None, False
    if mode == "init":
        return "sleep 0.1", False
    if mode == "random":
        return random.choice(ssh_commands), True
    return mode, False


def send_ssh_command(client, command_spec):
    # 执行 SSH 命令并读取输出
    if isinstance(command_spec, tuple):
        command, pre_sleep = command_spec
    else:
        command, pre_sleep = command_spec, False

    if not command:
        logger.info("Skipping SSH command execution.")
        return 0

    if pre_sleep:
        logger.info("Running pre-command sleep before `%s`.", command)
        sleep_stdout = None
        sleep_stderr = None
        _, sleep_stdout, sleep_stderr = client.exec_command("sleep 0.1")
        sleep_stdout.channel.recv_exit_status()
        sleep_stdout.read()
        sleep_stderr.read()

    _, stdout, stderr = client.exec_command(command)
    exit_code = stdout.channel.recv_exit_status()

    output = stdout.read().decode("utf-8", errors="replace")
    error_output = stderr.read().decode("utf-8", errors="replace")

    if output:
        logger.info("Command `%s` output:\n%s", command, output.rstrip())
    if error_output:
        logger.error("Command `%s` error output:\n%s", command, error_output.rstrip())

    return exit_code


def close_ssh(client):
    # 关闭 SSH 连接
    logger.info("Closing SSH connection.")
    client.close()


def run_ssh(host, port, username, password, command):
    # 兼容旧调用方式
    client = connect_ssh(host, port, username, password)
    try:
        return send_ssh_command(client, command)
    finally:
        close_ssh(client)


if __name__ == "__main__":
    # logging.basicConfig(
    #     level=logging.INFO,
    #     format="%(asctime)s %(levelname)s %(message)s",
    # )
    run_ssh(
        "192.168.153.2", 
        22, 
        "root", 
        "root", 
        pick_ssh_command("random")
    )
