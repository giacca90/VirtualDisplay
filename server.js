const fs = require('fs');
const net = require('net');
const express = require('express');
const WebSocket = require('ws');
const http = require('http');
const path = require('path');

const STREAM_PORT = 9999;      // Puerto TCP donde GStreamer envía el stream
const HTTP_PORT = 8000;        // Puerto HTTP y WebSocket

const app = express();
const server = http.createServer(app);

// WebSocket Server usando el mismo servidor HTTP
const wss = new WebSocket.Server({ server });

let clients = [];

wss.on('connection', (socket) => {
  clients.push(socket);
  console.log('🟢 Cliente WebSocket conectado');

  socket.on('close', () => {
    clients = clients.filter(s => s !== socket);
    console.log('🔴 Cliente desconectado');
  });
});

// TCP server para recibir video de GStreamer
const streamServer = net.createServer((socket) => {
  console.log('📡 GStreamer conectado');

  socket.on('data', (data) => {
    clients.forEach((ws) => {
      if (ws.readyState === WebSocket.OPEN) {
        ws.send(data);
      }
    });
  });

  socket.on('end', () => {
    console.log('📴 GStreamer desconectado');
  });
});

streamServer.listen(STREAM_PORT, () => {
  console.log(`🎥 Esperando stream en TCP puerto ${STREAM_PORT}`);
});

// Servir archivos estáticos desde carpeta "public"
app.use(express.static(path.join(__dirname, 'public')));

// Iniciar servidor HTTP + WS
server.listen(HTTP_PORT, () => {
  console.log(`🌐 Servidor HTTP y WebSocket corriendo en http://localhost:${HTTP_PORT}`);
});
