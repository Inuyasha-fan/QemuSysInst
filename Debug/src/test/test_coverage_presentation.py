# 覆盖率采集演示脚本，SSH 和 SMTP 混合随机发送，适合演示场景

import logging
import csv
import socket
import struct
import random
from pathlib import Path

import pexpect

from test_ssh import *
from test_smtp import *

logger = logging.getLogger(__name__)

# 全局配置
repo_root = Path(__file__).resolve().parents[3]
build_script = repo_root / "Simulation" / "scripts" / "build.sh"
log_file = repo_root / "Log" / "test_coverage.log"
coverage_host = "127.0.0.1"
coverage_port = 3111
console_username = "root"
console_password = "root"
shell_prompt_pattern = r"root@.*#"
coverage_recv_timeout = 6
coverage_random_rounds = 100
ssh_host = "192.168.153.2"
ssh_port = 22
ssh_username = "root"
ssh_password = "root"
smtp_host = "192.168.153.2"
smtp_port = 25
smtp_from_addr = "root@Liusy-Ubuntu"
smtp_to_addrs = ["root@debian-mips"]
smtp_subject = "Test SMTP"
phase_name_map = {
    "ssh": "SSH",
    "smtp": "SMTP",
    "equal": "未区分",
}


def terminal_info(message):
    # 仅输出到终端，不写入日志文件
    print(message, flush=True)


def send_bool(sock, value):
    # 发送一个控制字节给 coverage 插件
    sock.sendall(struct.pack("!B", 1 if value else 0))


def connect_coverage_socket():
    # 连接 coverage 插件的控制 socket
    sock = socket.create_connection((coverage_host, coverage_port), timeout=10)
    sock.settimeout(coverage_recv_timeout)
    return sock


def launch_qemu():
    # 启动 QEMU 覆盖率模式
    return pexpect.spawn(
        "bash",
        [str(build_script), "coverage"],
        cwd=str(repo_root),
        encoding="utf-8",
        timeout=180,
    )


def login_and_prepare_guest(child):
    # 登录虚拟机并完成基础网络配置
    logger.info("Waiting for login prompt.")
    child.expect("debian-mips login:")
    child.sendline(console_username)

    logger.info("Sending password.")
    child.expect("Password:")
    child.sendline(console_password)

    logger.info("Waiting for shell prompt.")
    child.expect(shell_prompt_pattern)

    logger.info("Configuring network.")
    child.sendline("")
    child.expect(shell_prompt_pattern)
    child.sendline("ifconfig eth0 192.168.153.2 netmask 255.255.255.0 up")
    child.expect(shell_prompt_pattern)
    child.sendline("route add default gw 192.168.153.1 eth0")
    child.expect(shell_prompt_pattern)

    logger.info("Clearing /var/mail/user.")
    child.sendline("")
    child.expect(shell_prompt_pattern)
    child.sendline('echo -n > /var/mail/user')
    child.expect(shell_prompt_pattern)

    # logger.info("Running SSH and SMTP none-mode warmup.")
    # run_ssh(ssh_host, ssh_port, ssh_username, ssh_password, pick_ssh_command("none"))
    # run_smtp(smtp_host, smtp_port, smtp_from_addr, smtp_to_addrs, smtp_subject, pick_smtp_body("none"))

def shutdown_qemu(child):
    # 退出 QEMU 仿真
    logger.info("Entering QEMU console shutdown sequence.")
    child.sendline("")
    child.expect(shell_prompt_pattern)
    child.sendcontrol("a")
    child.send("c")
    child.expect(r"\(qemu\)")
    child.sendline("quit")
    child.expect(pexpect.EOF)


def load_coverage_hashes(csv_path, target_map):
    if not csv_path.exists():
        logger.warning("Coverage CSV not found: %s.", csv_path)
        return

    with csv_path.open("r", encoding="utf-8", newline="") as fp:
        reader = csv.reader(fp)
        next(reader, None)
        for row in reader:
            if not row:
                continue
            hash_text = row[0].strip()
            if not hash_text:
                continue
            try:
                hash_value = int(hash_text, 0)
            except ValueError:
                continue
            target_map.add(hash_value)

    logger.info("Loaded %s hashes from %s.", len(target_map), csv_path)


def wait_for_coverage_csv(coverage_sock, coverage_round, target_map):
    coverage_sock.settimeout(coverage_recv_timeout)
    csv_path = repo_root / "Data" / f"coverage-{coverage_round}.csv"

    while True:
        try:
            data = coverage_sock.recv(1)
            if not data:
                return False
            if data[0] != 1:
                continue

            logger.info("Received coverage completion for round %s.", coverage_round)
            load_coverage_hashes(csv_path, target_map)
            return True
        except socket.timeout:
            return False
        except OSError:
            return False


def append_ratio_row(rows, phase, coverage_round, current_map, baseline_ssh, baseline_smtp):
    current_vs_init_ssh = calc_shared_ratio(current_map, baseline_ssh)
    current_vs_init_smtp = calc_shared_ratio(current_map, baseline_smtp)
    if current_vs_init_ssh > current_vs_init_smtp:
        predicted_phase = "ssh"
    elif current_vs_init_ssh < current_vs_init_smtp:
        predicted_phase = "smtp"
    else:
        predicted_phase = "equal"

    rows.append({
        "actual_phase": phase,
        "predicted_phase": predicted_phase,
        "round": coverage_round,
        "current_vs_init_ssh": current_vs_init_ssh,
        "current_vs_init_smtp": current_vs_init_smtp,
    })


def calc_shared_ratio(left_map, right_map):
    if not right_map:
        return 0.0
    return len(left_map & right_map) / len(right_map)


def calc_prediction_accuracy_for_phase(rows, phase):
    phase_rows = [row for row in rows if row["actual_phase"] == phase]
    if not phase_rows:
        return 0.0
    matched = sum(1 for row in phase_rows if row["predicted_phase"] == phase)
    return matched / len(phase_rows)


def format_percent(value):
    return f"{value * 100:.2f}%"


def log_coverage_ratio_rows(rows):
    logger.info("actual_phase, predicted_phase, round, current_vs_init_ssh, current_vs_init_smtp")
    for row in rows:
        logger.info(
            "%s, %s, %s, %s, %s",
            row["actual_phase"],
            row["predicted_phase"],
            row["round"],
            row["current_vs_init_ssh"],
            row["current_vs_init_smtp"],
        )


def collect_rounds(coverage_sock, phase, mode, rounds, coverage_round, current_map, baseline_ssh, baseline_smtp, rows=None):
    if phase == "ssh":
        client = connect_ssh(ssh_host, ssh_port, ssh_username, ssh_password)
        close_client = close_ssh
    else:
        client = connect_smtp(smtp_host, smtp_port)
        close_client = close_smtp

    for index in range(rounds):
        current_map.clear()
        send_bool(coverage_sock, 1)
        if phase == "ssh":
            send_ssh_command(client, pick_ssh_command(mode))
        else:
            send_smtp_message(client, smtp_from_addr, smtp_to_addrs, smtp_subject, pick_smtp_body(mode))
        coverage_round += 1
        if index == rounds - 1:
            close_client(client)
            client = None
            send_bool(coverage_sock, 0)
        else:
            send_bool(coverage_sock, 0)

        if not wait_for_coverage_csv(coverage_sock, coverage_round, current_map):
            logger.warning("Timed out waiting for %s coverage CSV round %s.", phase.upper(), coverage_round)
            continue

        if rows is not None:
            append_ratio_row(rows, phase, coverage_round, current_map, baseline_ssh, baseline_smtp)

    if client is not None:
        close_client(client)

    return coverage_round


def collect_random_rounds_mixed(coverage_sock, rounds, coverage_round, baseline_ssh, baseline_smtp, rows):
    random_plan = ["ssh"] * rounds + ["smtp"] * rounds
    random.shuffle(random_plan)
    ssh_client = connect_ssh(ssh_host, ssh_port, ssh_username, ssh_password)
    smtp_client = connect_smtp(smtp_host, smtp_port)

    try:
        for random_index, phase in enumerate(random_plan, start=1):
            current_map = set()
            send_bool(coverage_sock, 1)

            if phase == "ssh":
                payload = pick_ssh_command("random")
                send_ssh_command(ssh_client, payload)
                command_text = payload[0] if isinstance(payload, tuple) else payload
                payload_desc = f"执行 `{command_text}` 指令"
            else:
                payload = pick_smtp_body("random")
                send_smtp_message(smtp_client, smtp_from_addr, smtp_to_addrs, smtp_subject, payload)
                payload_desc = f"发送 {len(payload)} 个字符的随机消息"

            coverage_round += 1
            send_bool(coverage_sock, 0)

            if not wait_for_coverage_csv(coverage_sock, coverage_round, current_map):
                logger.warning("Timed out waiting for %s coverage CSV round %s.", phase.upper(), coverage_round)
                terminal_info(
                    f"测试信息：第 {random_index} 个数据包为 {phase_name_map[phase]} 协议，发送数据为{payload_desc}，结果超时，未完成判定 ⏱"
                )
                continue

            append_ratio_row(rows, phase, coverage_round, current_map, baseline_ssh, baseline_smtp)
            predicted_phase = rows[-1]["predicted_phase"]
            verdict_text = "正确✅" if predicted_phase == phase else "错误❌"
            terminal_info(
                f"测试信息：第 {random_index} 个数据包为 {phase_name_map[phase]} 协议，发送数据为{payload_desc}，"
                f"判定为 {phase_name_map.get(predicted_phase, predicted_phase)}，判定{verdict_text}"
            )
    finally:
        close_ssh(ssh_client)
        close_smtp(smtp_client)

    return coverage_round


def run_coverage():
    # 初始化日志输出
    log_file.parent.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        handlers=[
            logging.FileHandler(log_file, mode="w", encoding="utf-8"),
        ],
        force=True,
    )

    child = None
    coverage_sock = None
    ssh_none_hashes = set()
    smtp_none_hashes = set()
    ssh_initial_hashes = set()
    smtp_initial_hashes = set()
    ratio_rows = []
    coverage_round = 0

    try:
        terminal_info("🚀 初始化信息：开始执行覆盖率测试")
        logger.info("Starting build script with coverage mode: %s.", build_script)
        terminal_info("⏳ 初始化信息：等待 QEMU 启动并出现登录提示")
        child = launch_qemu()
        login_and_prepare_guest(child)
        logger.info("Connecting to coverage socket.")
        coverage_sock = connect_coverage_socket()

        terminal_info("📦 初始化信息：发送初始包来收集初始覆盖率（SSH none、SMTP none、SSH init、SMTP init）")
        coverage_round = collect_rounds(coverage_sock, "ssh", "none", 1, coverage_round, ssh_none_hashes, ssh_initial_hashes, smtp_initial_hashes, None)
        coverage_round = collect_rounds(coverage_sock, "smtp", "none", 1, coverage_round, smtp_none_hashes, ssh_initial_hashes, smtp_initial_hashes, None)

        coverage_round = collect_rounds(coverage_sock, "ssh", "init", 1, coverage_round, ssh_initial_hashes, ssh_initial_hashes, smtp_initial_hashes, None)
        coverage_round = collect_rounds(coverage_sock, "smtp", "init", 1, coverage_round, smtp_initial_hashes, ssh_initial_hashes, smtp_initial_hashes, None)

        terminal_info(
            f"🎲 测试信息：开始随机发送协议包（SSH {coverage_random_rounds} 个、SMTP {coverage_random_rounds} 个，顺序随机）"
        )
        coverage_round = collect_random_rounds_mixed(
            coverage_sock,
            coverage_random_rounds,
            coverage_round,
            ssh_initial_hashes,
            smtp_initial_hashes,
            ratio_rows,
        )

    finally:
        if coverage_sock is not None:
            try:
                coverage_sock.close()
            except OSError:
                pass
        if child is not None:
            try:
                shutdown_qemu(child)
            except Exception:
                child.close(force=True)

    log_coverage_ratio_rows(ratio_rows)
    logger.info(
        "none mode recorded %s ssh hashes and %s smtp hashes.",
        len(ssh_none_hashes),
        len(smtp_none_hashes),
    )
    logger.info(
        "init mode recorded %s ssh hashes and %s smtp hashes.",
        len(ssh_initial_hashes),
        len(smtp_initial_hashes),
    )
    logger.info(
        "ssh none/init shared hashes: %s, ratio against init: %s.",
        len(ssh_none_hashes & ssh_initial_hashes),
        format_percent(calc_shared_ratio(ssh_none_hashes, ssh_initial_hashes)),
    )
    logger.info(
        "smtp none/init shared hashes: %s, ratio against init: %s.",
        len(smtp_none_hashes & smtp_initial_hashes),
        format_percent(calc_shared_ratio(smtp_none_hashes, smtp_initial_hashes)),
    )
    logger.info(
        "ssh prediction accuracy: %s.",
        format_percent(calc_prediction_accuracy_for_phase(ratio_rows, "ssh")),
    )
    logger.info(
        "smtp prediction accuracy: %s.",
        format_percent(calc_prediction_accuracy_for_phase(ratio_rows, "smtp")),
    )
    terminal_info(
        f"📊 测试结果：SSH 协议判定率 {format_percent(calc_prediction_accuracy_for_phase(ratio_rows, 'ssh'))}"
    )
    terminal_info(
        f"📊 测试结果：SMTP 协议判定率 {format_percent(calc_prediction_accuracy_for_phase(ratio_rows, 'smtp'))}"
    )


if __name__ == "__main__":
    run_coverage()
