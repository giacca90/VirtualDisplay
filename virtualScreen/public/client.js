// public/client.js
const video = document.getElementById('stream');
const container = document.getElementById('container');
let pc;
const info = document.getElementById('info');
let ws;

function connect() {
	ws = new WebSocket(`ws://${location.hostname}:8000/ws`);

	ws.addEventListener('open', () => {
		console.log('🔌 WS abierto, registro client');
		ws.send(JSON.stringify({type: 'client'}));
		info.innerHTML = 'Conectando al servidor...';
	});

	ws.addEventListener('close', () => {
		console.log('❌ WS cerrado, intentando reconectar en 2s...');
		info.style.visibility = 'visible';
		info.innerText = 'Conexión perdida. Intentando reconectar...';

		if (pc) {
			pc.close();
			pc = null;
		}

		setTimeout(connect, 2000);
	});

	ws.addEventListener('error', (err) => {
		console.error('⚠️ Error en WS:', err);
		ws.close();
	});

	ws.addEventListener('message', async (ev) => {
		const msg = JSON.parse(ev.data);
		console.log('⬅️ Recibido en browser:', msg);

		if (msg.type === 'error' && msg.code === 404) {
			if (info) {
				info.style.visibility = 'visible';
				info.innerText = 'Error del servidor: ' + msg.message;
			}
			return;
		}

		if (!pc) {
			pc = new RTCPeerConnection({iceServers: [{urls: 'stun:stun.l.google.com:19302'}]});

			// ...después de crear pc...
			let lastPacketsLost = 0;
			let lastTimestamp = 0;
			let goodCount = 0; // fuera de la función, para llevar el control

			async function monitorQuality() {
				if (!pc) return;
				const stats = await pc.getStats();
				let lostNow = 0;
				let currentRTT = 0;

				stats.forEach((report) => {
					// 1. Monitoreo de Latencia (RTT)
					if (report.type === 'candidate-pair' && report.state === 'succeeded' && report.writable) {
						currentRTT = (report.currentRoundTripTime || 0) * 1000; // Convertir a ms
					}

					// 2. Monitoreo de Pérdida de Paquetes
					if (report.type === 'inbound-rtp' && report.kind === 'video') {
						const packetsLost = report.packetsLost || 0;
						const timestamp = report.timestamp || 0;

						if (lastTimestamp && timestamp > lastTimestamp) {
							const lost = packetsLost - lastPacketsLost;
							lostNow = lost;

							// Lógica combinada: Bajar si hay mucha pérdida O mucha latencia
							if (lost > 10 || currentRTT > 150) {
								if (ws.readyState === WebSocket.OPEN) {
									ws.send(JSON.stringify({type: 'quality', action: 'lower'}));
									console.log(`⚠️ Bajando calidad: Pérdida=${lost}, RTT=${currentRTT.toFixed(1)}ms`);
								}
								goodCount = 0;
							}
							// Subir solo si AMBOS son excelentes
							else if (lost === 0 && currentRTT < 80) {
								goodCount++;
								if (goodCount > 10) {
									if (ws.readyState === WebSocket.OPEN) {
										ws.send(JSON.stringify({type: 'quality', action: 'raise'}));
										console.log(`✅ Subiendo calidad: RTT estable en ${currentRTT.toFixed(1)}ms`);
									}
									goodCount = 0;
								}
							} else {
								goodCount = 0;
							}
						}
						lastPacketsLost = packetsLost;
						lastTimestamp = timestamp;
					}
				});
				setTimeout(monitorQuality, 2000);
			}

			// Manejar el ACK del servidor para ver el bitrate actual en consola
			ws.addEventListener('message', (ev) => {
				const msg = JSON.parse(ev.data);
				if (msg.type === 'quality_ack') {
					console.log(`📊 Bitrate actual del servidor: ${msg.bitrate} kbps`);
				}
			});

			// Llama a monitorQuality cuando el stream esté activo
			pc.ontrack = (e) => {
				if (video && e.streams[0]) {
					video.srcObject = e.streams[0];
					video.autoplay = true;
					video.playsInline = true;

					info.style.visibility = 'hidden';

					video.onloadedmetadata = async () => {
						try {
							await video.play();
							console.log('▶️ Reproducción automática iniciada');

							if (container.requestFullscreen) {
								await container.requestFullscreen();
								console.log('🖥️ Pantalla completa activada');
							} else if (container.webkitRequestFullscreen) {
								// Safari
								await container.webkitRequestFullscreen();
							} else if (container.msRequestFullscreen) {
								// IE
								await container.msRequestFullscreen();
							}
						} catch (err) {
							console.warn('⚠️ Error al reproducir o entrar a pantalla completa:', err);
						}
					};

					console.log('▶️ Stream entrante asignado al <video>');
				}
				monitorQuality();
			};

			pc.onicecandidate = (e) => {
				if (e.candidate && ws.readyState === WebSocket.OPEN) {
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
}

connect();
