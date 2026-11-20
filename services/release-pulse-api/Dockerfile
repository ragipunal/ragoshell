# Self-hosted RagoShell Release Check & Anonymous Usage Pulse API
# Deploy behind an HTTPS reverse proxy (Caddy, Nginx, Traefik, Cloudflare Tunnel).
FROM node:24-alpine

RUN addgroup -S ragopulse && adduser -S ragopulse -G ragopulse
WORKDIR /app

COPY --chown=ragopulse:ragopulse server.mjs releases.json ./
RUN mkdir -p /app/data && chown -R ragopulse:ragopulse /app/data

USER ragopulse
ENV NODE_ENV=production \
    PULSE_BIND_HOST=127.0.0.1 \
    PULSE_PORT=8787 \
    PULSE_DB_PATH=/app/data/pulse.sqlite \
    PULSE_RELEASES_PATH=/app/releases.json

EXPOSE 8787
CMD ["node", "server.mjs"]
