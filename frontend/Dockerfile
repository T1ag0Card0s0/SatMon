FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1

WORKDIR /app

COPY index.html app.js styles.css server.py ./

EXPOSE 5173

CMD ["python", "-u", "server.py"]