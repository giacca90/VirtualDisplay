function websocketServerConnect() {
  const wsUrl = `ws://${location.hostname}:8000`; // Asegúrate de que coincida con tu server.js
  const canvas = document.createElement('canvas');
  document.body.appendChild(canvas);

  const player = new JSMpeg.Player(wsUrl, { canvas: canvas, autoplay: true, audio: false });
}
