# 1. Menggunakan image resmi Python versi slim yang ringan & cepat di-build
FROM python:3.11-slim

# 2. Menyetel variabel lingkungan Python agar tidak menulis file cache (.pyc)
ENV PYTHONDONTWRITEBYTECODE 1
ENV PYTHONUNBUFFERED 1

# 3. Menetapkan folder kerja utama di dalam container
WORKDIR /app

# 4. Menyalin berkas dependensi (requirements.txt) terlebih dahulu
COPY backend/requirements.txt ./backend/

# 5. Menginstal seluruh dependensi pustaka Python (Flask, Paho-MQTT, Gunicorn, dll)
RUN pip install --no-cache-dir -r backend/requirements.txt

# 6. Menyalin seluruh sisa berkas kode proyek Anda ke dalam container
COPY . .

# 7. Membuka port 7860 (Port wajib yang harus digunakan di Hugging Face Spaces)
EXPOSE 7860

# 8. Menjalankan web server Gunicorn pada port 7860 secara production-ready
CMD ["gunicorn", "--chdir", "backend", "--bind", "0.0.0.0:7860", "app:app"]