# Nexus-Office en contenedor (servidores, NAS, nube). Multi-arquitectura: linux/amd64 y linux/arm64.
#   docker build -t nexus-office .
#   docker run -d -p 8775:8775 -p 8766:8766 -v nexus-data:/app/data --name nexus nexus-office
# GPU NVIDIA: anade --gpus all y fija hardware.profile_override=cuda (Sistema > Ajustes admin).
FROM python:3.11-slim-bookworm

ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 NEXUS_DATA_DIR=/app/data
RUN apt-get update && apt-get install -y --no-install-recommends libgomp1 libvulkan1 ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir --only-binary=:all: -r requirements.txt
COPY nexus ./nexus
COPY README.md LICENSE ./
RUN python -c "import yaml,pathlib; p=pathlib.Path('/app/defaults.yaml'); p.write_text(yaml.safe_dump({'server': {'host': '0.0.0.0', 'open_browser': False}}))"
VOLUME ["/app/data"]
EXPOSE 8775 8766
HEALTHCHECK --interval=30s --timeout=5s CMD curl -fs http://127.0.0.1:8775/api/status || exit 1
CMD ["sh", "-c", "[ -f /app/data/config.yaml ] || cp /app/defaults.yaml /app/data/config.yaml; exec python -m nexus run --host 0.0.0.0 --no-browser"]
