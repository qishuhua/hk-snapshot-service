FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
# 换华为云源（noble 用 deb822 格式的 ubuntu.sources）
RUN sed -i 's|http://archive.ubuntu.com/ubuntu|http://repo.huaweicloud.com/ubuntu|g; s|http://security.ubuntu.com/ubuntu|http://repo.huaweicloud.com/ubuntu|g' /etc/apt/sources.list.d/ubuntu.sources \
    && apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config \
        libavcodec-dev libavutil-dev libswscale-dev \
        nlohmann-json3-dev libcpp-httplib-dev libspdlog-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY third_party ./third_party

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)"

# ---------------------------------------------------------------------------
# 运行镜像：仅 FFmpeg 运行库（noble/24.04 = FFmpeg 6.1：soname 60/58/7）+ HCNetSDK so
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
# 换华为云源（与 build stage 相同；runtime 也要装 FFmpeg 运行库）
RUN sed -i 's|http://archive.ubuntu.com/ubuntu|http://repo.huaweicloud.com/ubuntu|g; s|http://security.ubuntu.com/ubuntu|http://repo.huaweicloud.com/ubuntu|g' /etc/apt/sources.list.d/ubuntu.sources \
    && apt-get update && apt-get install -y --no-install-recommends \
        libavcodec60 libavutil58 libswscale7 \
        libcpp-httplib0.14t64 libspdlog1.12-fmt9 \
        ca-certificates curl tini \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --no-create-home capture \
    && mkdir -p /opt/hk-snapshot-service/bin /opt/hk-snapshot-service/lib /data/logs \
    && chown -R capture:capture /opt/hk-snapshot-service /data/logs

COPY --from=build /src/build/hk-snapshot-service /opt/hk-snapshot-service/bin/
COPY web /opt/hk-snapshot-service/web
# 整目录拷贝：libhcnetsdk.so 与 HCNetSDKCom/ 组件的相对布局必须保持
COPY --from=build /src/third_party/HCNetSDK/lib /opt/hk-snapshot-service/lib

ENV LD_LIBRARY_PATH=/opt/hk-snapshot-service/lib:/opt/hk-snapshot-service/lib/HCNetSDKCom \
    HKMEDIA_PORT=8180

USER capture
WORKDIR /opt/hk-snapshot-service
EXPOSE 8180

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -fsS "http://127.0.0.1:${HKMEDIA_PORT}/healthz" || exit 1

ENTRYPOINT ["/usr/bin/tini", "--", "/opt/hk-snapshot-service/bin/hk-snapshot-service"]
