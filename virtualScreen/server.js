const express = require('express');
const os = require('node:os');
const {spawn} = require('node:child_process');
const WebSocket = require('ws');
const http = require('node:http');
const path = require('node:path');

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

		data = JSON.parse(raw);
		if (ws === peer || ws === gstreamer) {
			console.log('⬇️ Mensaje recibido desde ' + (ws === peer ? 'peer' : 'gstreamer') + ': ', data);
		} else {
			console.log('⬇️ Primer mensaje recibido:', data);
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
	console.log(`🌍 Servidor WebRTC en ${getLocalIPAddress()}:${HTTP_PORT}`);
	console.log('🚀 Iniciando proceso WebRTC...');
	let webrtcProcess = spawn('./webrtc_screen', {
		env: {
			...process.env,
			GST_DEBUG: '3',
			// GST_DEBUG: ':3',
			// GST_DEBUG: 'ximagesrc:4',
			// GST_DEBUG: 'queue:3',
			// GST_DEBUG: 'videoconvert:3',
			// GST_DEBUG: 'vaapih264enc:3,GST_PADS:1'
			// GST_DEBUG: 'h264parse:3',
			// GST_DEBUG: 'rtph264pay:3',
			// GST_debug: 'webrtcbin:3',
		},
	});

	webrtcProcess.stdout.setEncoding('utf8');
	webrtcProcess.stderr.setEncoding('utf8');

	webrtcProcess.stdout.on('data', (data) => {
		process.stdout.write(`🟢 WebRTC: ${data}`);
	});

	webrtcProcess.stderr.on('data', (data) => {
		process.stderr.write(`🔴 WebRTC: ${data}`);
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
