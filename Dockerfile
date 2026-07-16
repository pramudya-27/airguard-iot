# Dockerfile (letakkan di root folder)
# -----------------------------------------------------------------
# Use the official Python image (slim)
FROM python:3.11-slim

ENV PYTHONDONTWRITEBYTECODE 1
ENV PYTHONUNBUFFERED 1

WORKDIR /app

# copy only requirements first (cache‑friendly)
COPY backend/requirements.txt ./backend/

# install dependencies
RUN pip install --no-cache-dir -r backend/requirements.txt

# copy seluruh kode
COPY . .

# Vercel memberi port lewat $PORT, expose saja untuk dokumentasi
EXPOSE 8080

# Jalankan Flask via Gunicorn, pakai $PORT yang disediakan Vercel
CMD ["gunicorn", "--chdir", "backend", "--bind", "0.0.0.0:$PORT", "app:app"]
# -----------------------------------------------------------------