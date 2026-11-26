# ragoshell/web — self-hosted Mod B gateway (docs/WebGateway.md §5).
#
# Build from the REPO ROOT, after producing the WASM bundle locally:
#   cmake --preset wasm-single && cmake --build --preset wasm-single
#   docker build -f src/ragoshell-web-gateway/docker/Dockerfile -t ragoshell/web .
#
# The image contains ONLY the gateway + static web bundle. TLS terminates at
# a reverse proxy in front (the gateway speaks cleartext HTTP/WS on :8080 —
# see docs/WebGateway.md §3).

# ---------- builder ----------
FROM ubuntu:24.04 AS builder
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates git curl unzip zip tar pkg-config \
        build-essential ninja-build cmake python3 python3-pip \
        autoconf automake autoconf-archive libtool bison flex \
        libgl1-mesa-dev libegl1-mesa-dev libxkbcommon-dev \
        libfontconfig1-dev libfreetype-dev libdbus-1-dev libglib2.0-dev \
    && rm -rf /var/lib/apt/lists/*

# QAbstractHttpServer::addWebSocketUpgradeVerifier needs Qt >= 6.8; Ubuntu
# 24.04 ships 6.4.2, so install the same pinned release dev builds use.
# aqtinstall and the vcpkg checkout are pinned for reproducibility —
# never track a mutable HEAD in a release image.
RUN pip3 install --no-cache-dir --break-system-packages "aqtinstall==3.3.0" \
 && aqt install-qt linux desktop 6.12.0 linux_gcc_64 --outputdir /opt/qt \
        --modules qthttpserver qtwebsockets qt5compat

# vcpkg builds mongo-cxx-driver / openssl / argon2 / libssh2 (static,
# x64-linux). Pinned to the commit the dev tree builds against.
RUN git clone https://github.com/microsoft/vcpkg /opt/vcpkg \
 && git -C /opt/vcpkg checkout 5363acb796e576328288fae60c25304e227e9839 \
 && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

WORKDIR /src
# Whitelist-COPY: only the files the build needs enter the context — a
# stray .env/id_rsa in a working tree can never leak into a layer.
# (RagoShellLicenses.cmake FATAL_ERRORs on missing root notices.)
COPY CMakeLists.txt vcpkg.json vcpkg-configuration.json ./
COPY LICENSE LICENSE.LGPLv3 COPYRIGHT THIRD-PARTY-NOTICES.txt ./
COPY assets/fonts/DEJAVU-LICENSE ./assets/fonts/DEJAVU-LICENSE
COPY cmake/ ./cmake/
COPY src/ ./src/
COPY vcpkg-overlays/ ./vcpkg-overlays/
RUN cmake -B build/gateway -G Ninja \
        -DRAGO_GATEWAY_ONLY=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=/opt/qt/6.12.0/gcc_64 \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_OVERLAY_PORTS=/src/vcpkg-overlays/ports \
        -DVCPKG_OVERLAY_TRIPLETS=/src/vcpkg-overlays/triplets \
 && cmake --build build/gateway --target ragoshell-web-gateway \
 && strip /src/build/gateway/src/ragoshell-app/ragoshell-web-gateway

# ---------- runtime (hosted: Mod B API-backed relay without WASM static assets) ----------
FROM ubuntu:24.04 AS hosted
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libgl1 libegl1 libopengl0 libxkbcommon0 libfontconfig1 libfreetype6 libglib2.0-0 libdbus-1-3 libgssapi-krb5-2 ca-certificates bash \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --create-home ragoshell \
    && mkdir -p /srv/web \
    && chmod 0755 /srv/web

COPY --from=builder /opt/qt/6.12.0/gcc_64/lib /opt/qt/6.12.0/gcc_64/lib
COPY --from=builder /opt/qt/6.12.0/gcc_64/plugins /opt/qt/6.12.0/gcc_64/plugins
COPY --from=builder /src/build/gateway/src/ragoshell-app/ragoshell-web-gateway \
     /usr/local/bin/ragoshell-web-gateway

ENV LD_LIBRARY_PATH=/opt/qt/6.12.0/gcc_64/lib \
    QT_QPA_PLATFORM=offscreen \
    RAGOSHELL_WEB_ROOT=/srv/web \
    RAGOSHELL_GW_BIND=0.0.0.0 \
    RAGOSHELL_GW_PORT=8080
USER ragoshell
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/ragoshell-web-gateway"]

# ---------- runtime (web: default self-hosted bundle with WASM static assets) ----------
FROM hosted AS web
USER root
# Static WASM bundle produced by the local wasm-single build (renamed to
# index.html so / serves the app shell directly).
COPY build/wasm-single/src/ragoshell-app/ragoshell.html   /srv/web/index.html
COPY build/wasm-single/src/ragoshell-app/ragoshell.js     /srv/web/ragoshell.js
COPY build/wasm-single/src/ragoshell-app/ragoshell.wasm   /srv/web/ragoshell.wasm
COPY build/wasm-single/src/ragoshell-app/ragoshell.wasm.br /srv/web/ragoshell.wasm.br
COPY build/wasm-single/src/ragoshell-app/qtloader.js      /srv/web/qtloader.js
COPY build/wasm-single/src/ragoshell-app/qtlogo.svg       /srv/web/qtlogo.svg
USER ragoshell
