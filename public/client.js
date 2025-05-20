// public/client.js
const video = document.getElementById('stream');
let pc;

// Conecta y registra
const ws = new WebSocket(`ws://${location.hostname}:8000/ws`);
ws.addEventListener('open', () => {
	console.log('🔌 WS abierto, registro client');
	ws.send(JSON.stringify({type: 'client'}));
});

ws.addEventListener('message', async (ev) => {
	const msg = JSON.parse(ev.data);
	console.log('⬅️ Recibido en browser:', msg);

	if (!pc) {
		pc = new RTCPeerConnection({iceServers: [{urls: 'stun:stun.l.google.com:19302'}]});

		pc.onicecandidate = (e) => {
			if (e.candidate) {
				ws.send(
					JSON.stringify({
						type: 'ice',
						sdpMLineIndex: e.candidate.sdpMLineIndex,
						candidate: e.candidate.candidate,
					}),
				);
				console.log('ICE candidate enviado:', e.candidate);
			}
		};
		pc.ontrack = (e) => {
			if (video && e.streams[0]) {
				video.srcObject = e.streams[0];
				video.autoplay = true;
				video.playsInline = true;

				video.onloadedmetadata = async () => {
					try {
						await video.play();
						console.log('▶️ Reproducción automática iniciada');

						if (video.requestFullscreen) {
							await video.requestFullscreen();
							console.log('🖥️ Pantalla completa activada');
						} else if (video.webkitRequestFullscreen) {
							// Safari
							await video.webkitRequestFullscreen();
						} else if (video.msRequestFullscreen) {
							// IE
							await video.msRequestFullscreen();
						}
					} catch (err) {
						console.warn('⚠️ Error al reproducir o entrar a pantalla completa:', err);
					}
				};

				console.log('▶️ Stream entrante asignado al <video>');
			}
		};
	}
	if (msg.type === 'ack' && msg.role === 'client') {
		ws.send(JSON.stringify({type: 'ready'}));
		console.log('📨 Enviado: ready');
	}

	if (msg.type === 'offer') {
		console.log('▶️ Setting remote offer');
		await pc.setRemoteDescription(new RTCSessionDescription(msg));
		const answer = await pc.createAnswer();
		await pc.setLocalDescription(answer);
		console.log('▶️ Enviando answer');
		ws.send(JSON.stringify({type: 'answer', sdp: pc.localDescription.sdp}));
	} else if (msg.type === 'ice' && msg.candidate) {
		console.log('▶️ Agregando ICE candidate');
		await pc.addIceCandidate(
			new RTCIceCandidate({
				candidate: msg.candidate,
				sdpMLineIndex: msg.sdpMLineIndex,
			}),
		);
	}
});
