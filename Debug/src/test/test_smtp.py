# SMTP 邮件发送测试脚本，支持 none/init/random 三种模式用于覆盖率采集

import logging
import random
import string
import smtplib
from email.message import EmailMessage


logger = logging.getLogger(__name__)


def pick_smtp_body(mode):
    # 根据模式决定邮件正文
    if mode == "none":
        return None
    if mode == "init":
        return ""
    if mode == "random":
        length = random.randint(1, 64)
        ascii_chars = string.ascii_letters + string.digits + string.punctuation + " "
        return "".join(random.choice(ascii_chars) for _ in range(length))
    return mode


def connect_smtp(smtp_host, smtp_port):
    # 建立 SMTP 连接
    client = smtplib.SMTP(timeout=10)
    logger.info("Connecting to %s:%s.", smtp_host, smtp_port)
    client.connect(host=smtp_host, port=smtp_port)
    client.ehlo()
    return client


def send_smtp_message(client, from_addr, to_addrs, subject, body):
    # 发送一封邮件
    if body is None:
        logger.info("Skipping SMTP message send.")
        return 0

    message = EmailMessage()
    message["From"] = from_addr
    message["To"] = ", ".join(to_addrs)
    message["Subject"] = subject
    message.set_content(body)

    logger.info("Sending email from %s to %s.", from_addr, ", ".join(to_addrs))
    client.send_message(message)
    logger.info("Email sent successfully.")
    return 0


def close_smtp(client):
    # 关闭 SMTP 连接
    logger.info("Closing SMTP connection.")
    try:
        client.quit()
    except smtplib.SMTPException:
        client.close()


def run_smtp(smtp_host, smtp_port, from_addr, to_addrs, subject, body):
    # 兼容旧调用方式
    client = connect_smtp(smtp_host, smtp_port)
    try:
        send_smtp_message(client, from_addr, to_addrs, subject, body)
        return 0
    finally:
        close_smtp(client)


if __name__ == "__main__":
    # logging.basicConfig(
    #     level=logging.INFO,
    #     format="%(asctime)s %(levelname)s %(message)s",
    # )
    run_smtp(
        "192.168.153.2",
        25,
        "root@Liusy-Ubuntu",
        ["root@debian-mips"],
        "Test SMTP",
        pick_smtp_body("random"),
    )
