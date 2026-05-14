const express = require('express');
const os = require('node:os');
const {spawn} = require('node:child_process');
const crypto = require('node:crypto');
const WebSocket = require('ws');
const http = require('node:http');
const path = require('node:path');

const HTTP_PORT = 8000;

// Valida que el SDP answer sea válido para WebRTC
function isValidAnswerSDP(sdp) {
	if (!sdp) return false;
	// Buscamos m=video 0 en cualquier parte del string
	if (/m=video 0 /i.test(sdp)) {
		console.warn(`❌ SDP rechazado: El navegador ha desactivado el video (puerto 0)`);
		return false;
	}
	return true;
}

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({server, path: '/ws'});

const peers = new Map(); // múltiples clientes navegador
let gstreamer = null; // cliente GStreamer

wss.on('connection', (ws) => {
	console.log('🔌 Nuevo WebSocket conectado');

	ws.on('message', (raw) => {
		let data = JSON.parse(raw);
		if (ws === gstreamer) {
			console.log('⬇️ Mensaje recibido desde GStreamer:', data);
		} else if (ws.peerId) {
			console.log(`⬇️ Mensaje recibido desde cliente ${ws.peerId}:`, data);
		} else {
			console.log('⬇️ Primer mensaje recibido:', data);
		}

		// Identificación inicial
		if (data.type === 'client') {
			const peerId = crypto.randomUUID();
			ws.peerId = peerId;
			peers.set(peerId, ws);
			console.log(`🎥 Cliente navegador registrado: ${peerId}`);
			ws.send(JSON.stringify({type: 'ack', role: 'client', peer_id: peerId}));
			return;
		}
		if (data.type === 'gstreamer') {
			console.log('📡 GStreamer registrado');
			gstreamer = ws;
			ws.send(JSON.stringify({type: 'ack', role: 'gstreamer'}));
			return;
		}

		// Reenvío SDP/ICE
		if (ws !== gstreamer && gstreamer && ['ready', 'answer', 'ice', 'quality'].includes(data.type)) {
			// Validar answer SDP antes de reenviar a GStreamer
			if (data.type === 'answer' && data.sdp && !isValidAnswerSDP(data.sdp)) {
				console.error(`❌ RECHAZANDO answer SDP inválido del cliente ${ws.peerId}`);
				ws.send(JSON.stringify({type: 'error', message: 'Media rejected by browser (port 0)'}));
				// Informamos a GStreamer para que libere los recursos de este cliente
				if (gstreamer) gstreamer.send(JSON.stringify({type: 'remove', peer_id: ws.peerId}));
				return;
			}
			const dataToSend = JSON.stringify(data);
			console.log('→ Reenvío a GStreamer:', dataToSend);
			gstreamer.send(dataToSend);
		} else if (ws === gstreamer && data.peer_id) {
			const targetPeer = peers.get(data.peer_id);
			if (targetPeer && targetPeer.readyState === WebSocket.OPEN) {
				const dataToSend = JSON.stringify(data);
				console.log(`→ Reenvío a navegador ${data.peer_id}:`, dataToSend);
				targetPeer.send(dataToSend);
			} else {
				console.warn(`⚠️ Peer no encontrado o cerrado: ${data.peer_id}`);
			}
		}
	});

	ws.on('close', () => {
		if (ws.peerId) {
			console.log(`❌ Cliente desconectado: ${ws.peerId}`);
			peers.delete(ws.peerId);
			// Informamos a GStreamer para que elimine al cliente y no bloquee el tee
			if (gstreamer) gstreamer.send(JSON.stringify({type: 'remove', peer_id: ws.peerId}));
		}
		if (ws === gstreamer) {
			console.log('❌ GStreamer desconectado');
			gstreamer = null;
		}
	});
});

// Servir archivos estáticos en /public
app.use(express.static(path.join(__dirname, 'public')));

let webrtcProcess = null;

server.listen(HTTP_PORT, () => {
	console.log(`🌍 Servidor WebRTC en ${getLocalIPAddress()}:${HTTP_PORT}`);
	console.log('🚀 Iniciando proceso WebRTC...');
	webrtcProcess = spawn('./webrtc_screen', {
		env: {...process.env, GST_DEBUG: '3'},
	});

	webrtcProcess.stdout.setEncoding('utf8');
	webrtcProcess.stderr.setEncoding('utf8');

	webrtcProcess.stdout.on('data', (data) => {
		process.stdout.write(`🟢 WebRTC stdout: ${data}`);
	});

	webrtcProcess.stderr.on('data', (data) => {
		process.stderr.write(`🔴 WebRTC stderr: ${data}`);
	});

	webrtcProcess.on('close', (code) => {
		console.log(`❌ WebRTC salió con código ${code}`);
		webrtcProcess = null;
	});
});

// Función que detecta la IP local real
function getLocalIPAddress() {
	const interfaces = os.networkInterfaces();
	for (const iface of Object.values(interfaces)) {
		for (const config of iface) {
			if (config.family === 'IPv4' && !config.internal) {
				return config.address;
			}
		}
	}
	return 'localhost';
}

function shutdown() {
	console.log('\n🛑 Apagando servidor...');
	if (webrtcProcess) {
		console.log('🧹 Terminando proceso WebRTC...');
		webrtcProcess.kill('SIGINT');
	}
	server.close(() => {
		console.log('✅ Servidor cerrado');
		process.exit(0);
	});
}

// Manejar señales de salida
process.on('SIGINT', shutdown); // Ctrl+C
process.on('SIGTERM', shutdown); // kill o systemd
process.on('exit', shutdown);
