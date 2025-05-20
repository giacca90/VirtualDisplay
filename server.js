const express = require('express');
const WebSocket = require('ws');
const http = require('http');
const path = require('path');

const HTTP_PORT = 8000;

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({server, path: '/ws'});

let peer = null; // cliente navegador
let gstreamer = null; // cliente GStreamer

wss.on('connection', (ws) => {
	console.log('🔌 Nuevo WebSocket conectado');

	ws.on('message', (raw) => {
		let data;
		try {
			data = JSON.parse(raw);
			if (ws === peer || ws === gstreamer) {
				console.log('⬇️ Mensaje recibido desde ' + (ws === peer ? 'peer' : 'gstreamer') + ': ', data);
			} else {
				console.log('⬇️ Primer mensaje recibido:', data);
			}
		} catch (e) {
			console.error('Mensaje no JSON:', raw);
			return;
		}

		// Identificación inicial
		if (data.type === 'client') {
			console.log('🎥 Cliente navegador registrado');
			peer = ws;
			ws.send(JSON.stringify({type: 'ack', role: 'client'}));
			return;
		}
		if (data.type === 'gstreamer') {
			console.log('📡 GStreamer registrado');
			gstreamer = ws;
			ws.send(JSON.stringify({type: 'ack', role: 'gstreamer'}));
			return;
		}

		// Reenvío SDP/ICE
		if (ws === peer && gstreamer) {
			const dataToSend = JSON.stringify(data);
			console.log('→ Reenvío a GStreamer:', dataToSend);
			gstreamer.send(dataToSend);
		} else if (ws === gstreamer && peer) {
			const dataToSend = JSON.stringify(data);
			console.log('→ Reenvío al navegador:', dataToSend);
			peer.send(dataToSend);
		}
	});

	ws.on('close', () => {
		if (ws === peer) {
			console.log('❌ Peer desconectado');
			peer = null;
		}
		if (ws === gstreamer) {
			console.log('❌ GStreamer desconectado');
			gstreamer = null;
		}
	});
});

// Servir archivos estáticos en /public
app.use(express.static(path.join(__dirname, 'public')));

server.listen(HTTP_PORT, () => {
	console.log(`🌍 Servidor WebRTC en http://localhost:${HTTP_PORT}`);
});
