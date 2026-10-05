FROM ubuntu:latest

ENV TZ=Asia/Seoul LANG=ko_KR.UTF-8 LANGUAGE=ko_KR.UTF-8

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    locales tzdata build-essential sudo gdb vim curl git wget \
    python3 cmake make gcc sudo valgrind telnet net-tools iproute2\
    && locale-gen ko_KR.UTF-8 && update-locale LANG=ko_KR.UTF-8

# 호스트(UID 1000)와 파일 권한 일치를 위해 기본 ubuntu 계정 정리 후 jungle(UID 1000) 생성
RUN touch /var/mail/ubuntu 2>/dev/null || true \
    && (id -u ubuntu &>/dev/null && userdel -r ubuntu 2>/dev/null || true) \
    && (groupadd -g 1000 jungle 2>/dev/null || true) \
    && useradd -u 1000 -g 1000 -m -s /bin/bash jungle \
    && usermod -aG sudo jungle
RUN echo "jungle ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/jungle
# 암호 설정
# RUN echo 'jungle:jungle' | chpasswd

USER jungle
WORKDIR /home/jungle
