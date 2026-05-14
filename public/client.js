// public/client.js
const video = document.getElementById('stream');
let pc;
let peerId;

function getDeviceName() {
	try {
		const ua = navigator.userAgent;
		let os = 'UnknownOS';
		let model = '';

		// Detección de Sistema Operativo
		if (/windows/i.test(ua)) os = 'Windows';
		else if (/android/i.test(ua)) os = 'Android';
		else if (/linux/i.test(ua)) os = 'Linux';
		else if (/iPhone|iPad|iPod/i.test(ua)) os = 'iOS';
		else if (/macintosh/i.test(ua)) os = 'macOS';

		// Intentar obtener modelo de dispositivo (especialmente en Android)
		if (/android/i.test(ua)) {
			const match = ua.match(/Android [^;]+; ([^;)]+)/);
			if (match) model = `_${match[1].split(' Build')[0]}`;
		} else if (/iPhone/i.test(ua)) model = '_iPhone';
		else if (/iPad/i.test(ua)) model = '_iPad';

		// Detección de Navegador
		let browser = 'Browser';
		if (/edg/i.test(ua)) browser = 'Edge';
		else if (/brave/i.test(ua) || (navigator.brave && navigator.brave.isBrave())) browser = 'Brave';
		else if (/chrome|crios/i.test(ua)) browser = 'Chrome';
		else if (/firefox|fxios/i.test(ua)) browser = 'Firefox';
		else if (/safari/i.test(ua) && !/chrome|crios/i.test(ua)) browser = 'Safari';
		else if (/opr|opera/i.test(ua)) browser = 'Opera';

		// Limpiar y formatear
		let name = `${os}${model}_${browser}`;
		return name.replace(/\s+/g, '_').replace(/[^a-zA-Z0-9_]/g, '');
	} catch (e) {
		return 'Device';
	}
}

// Conecta al servidor usando el host actual
const ws = new WebSocket(`ws://${window.location.host}/ws`);

ws.addEventListener('open', () => {
	console.log('🔌 WS abierto, registrando cliente...');
	ws.send(JSON.stringify({type: 'client', deviceName: getDeviceName()}));
});

ws.addEventListener('message', async (ev) => {
	const msg = JSON.parse(ev.data);
	console.log('⬅️ Recibido:', msg.type);

	if (msg.type === 'ack' && msg.role === 'client') {
		peerId = msg.peer_id;
		console.log('✅ Registrado con Peer ID:', peerId);
		// Notificamos que estamos listos para recibir oferta
		ws.send(JSON.stringify({type: 'ready', peer_id: peerId}));
	}

	if (msg.type === 'offer') {
		console.log('▶️ Recibida oferta SDP, negociando...');

		if (!pc) {
			pc = new RTCPeerConnection({
				iceServers: [{urls: 'stun:stun.l.google.com:19302'}],
				bundlePolicy: 'max-bundle',
			});

			// Forzamos la recepción de video
			pc.addTransceiver('video', {direction: 'recvonly'});

			pc.onicecandidate = (e) => {
				if (e.candidate && peerId) {
					ws.send(
						JSON.stringify({
							type: 'ice',
							peer_id: peerId,
							sdpMLineIndex: e.candidate.sdpMLineIndex,
							candidate: e.candidate.candidate,
						}),
					);
				}
			};

			pc.ontrack = (e) => {
				console.log('📺 Stream de video recibido');
				if (video && e.streams[0]) {
					video.srcObject = e.streams[0];
					video.play().catch((err) => console.warn('⚠️ Error al reproducir video:', err));
				}
			};

			pc.onconnectionstatechange = () => {
				console.log('⚡ Estado de conexión:', pc.connectionState);
				if (pc.connectionState === 'failed' || pc.connectionState === 'disconnected') {
					console.warn('❌ Conexión fallida, reiniciando...');
					location.reload();
				}
			};
		}

		try {
			await pc.setRemoteDescription(new RTCSessionDescription(msg));
			const answer = await pc.createAnswer();
			await pc.setLocalDescription(answer);

			// Verificación de seguridad: si el navegador rechaza el video, lo veremos aquí
			if (answer.sdp.includes('m=video 0')) {
				console.error('❌ El navegador ha rechazado el flujo de video (puerto 0 en SDP)');
			}

			ws.send(
				JSON.stringify({
					type: 'answer',
					sdp: pc.localDescription.sdp,
					peer_id: peerId,
				}),
			);
			console.log('📤 Respuesta SDP enviada');
		} catch (err) {
			console.error('❌ Error en la negociación SDP:', err);
		}
	}

	if (msg.type === 'ice' && msg.candidate && pc) {
		try {
			await pc.addIceCandidate(
				new RTCIceCandidate({
					candidate: msg.candidate,
					sdpMLineIndex: msg.sdpMLineIndex,
				}),
			);
		} catch (err) {
			console.warn('⚠️ Error al agregar ICE candidate:', err);
		}
	}
});

ws.addEventListener('close', () => {
	console.log('❌ Conexión WS cerrada. Recargando en 3s...');
	setTimeout(() => location.reload(), 3000);
});
