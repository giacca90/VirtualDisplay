#!/bin/bash
PLUGIN_DIR="$(dirname "$0")"
DESKTOP_FILE="$PLUGIN_DIR/Proyector Virtual.desktop"
# --- Tus iconos (rutas absolutas) ---
ICON_ON="/usr/share/icons/virtualScreenOn.png"
ICON_OFF="/usr/share/icons/virtualScreenOff.png"

log() { echo "[LOG] $1"; }
error_exit() {
	echo "[ERROR] $1"
	exit 1
}

xinput map-to-output "ELAN900C:00 04F3:2846" eDP-1

get_pids() {
	pgrep -f "node server.js"
	pgrep -f "\./webrtc_screen"
}

update_icon() {
	ICON="$1"
	if [ -w "$DESKTOP_FILE" ]; then
		if sed -i "s|^Icon=.*|Icon=$ICON|" "$DESKTOP_FILE"; then
			log "Icono actualizado correctamente a $ICON."
		else
			log "Error al actualizar el icono en $DESKTOP_FILE."
		fi
	else
		log "No hay permisos de escritura para $DESKTOP_FILE."
	fi
	# --- Refrescar icono en xfce4-panel con el icono recién puesto ---
	BASE_ICON=$(basename "$ICON" .png)
	CHANNEL="xfce4-panel"
	PROP_ROOT="/plugins"
	PLUGIN_ID=""

	for p in $(xfconf-query -c $CHANNEL -l | grep "^$PROP_ROOT/plugin-[0-9]\+/icon$"); do
		val=$(xfconf-query -c $CHANNEL -p "$p")
		if [[ "$val" == "$(basename "$ICON_ON" .png)" || "$val" == "$(basename "$ICON_OFF" .png)" ]]; then
			PLUGIN_ID=$(echo "$p" | sed -E 's#^/plugins/plugin-([0-9]+)/icon$#\1#')
			break
		fi
	done

	if [[ -n "$PLUGIN_ID" ]]; then
		PROP="/plugins/plugin-${PLUGIN_ID}/icon"
		old=$(xfconf-query -c $CHANNEL -p "$PROP")
		tmp="application-exit"
		[[ "$old" == "$tmp" ]] && tmp="system-run"
		xfconf-query -c $CHANNEL -p "$PROP" -s "$tmp"
		xfconf-query -c $CHANNEL -p "$PROP" -s "$BASE_ICON"
		log "✅ Icono de plugin-$PLUGIN_ID recargado (usando '$BASE_ICON')."
	else
		log "❌ No encontrado plugin con icono '$(basename "$ICON_ON" .png)' o '$(basename "$ICON_OFF" .png)' para refrescar."
	fi
}

mapfile -t PIDS < <(get_pids)

if [ ${#PIDS[@]} -gt 0 ]; then
	log "Procesos detectados (${PIDS[*]}). Matando..."
	for PID in "${PIDS[@]}"; do
		kill "$PID"
	done
	# Esperar y forzar si es necesario
	sleep 1
	for PID in "${PIDS[@]}"; do
		if kill -0 "$PID" 2>/dev/null; then
			log "El proceso $PID sigue vivo, forzando kill -9"
			kill -9 "$PID"
		fi
	done
	update_icon "$ICON_OFF"
	log "Todos los procesos detenidos. Icono OFF."
	read -p "Presiona ENTER para cerrar..."
	exit 0
else
	log "No hay procesos activos. Iniciando servidor Node..."
	cd "$PLUGIN_DIR" || error_exit "No se pudo acceder a $PLUGIN_DIR"
	node server.js &
	update_icon "$ICON_ON"
	log "Servidor iniciado. Icono ON."
	# Mantener el script abierto mientras el servidor está en marcha
	while pgrep -f "node server.js" >/dev/null 2>&1; do
		sleep 2
	done
	update_icon "$ICON_OFF"
	log "Servidor Node finalizado. Icono OFF."
	read -p "Presiona ENTER para cerrar..."
	exit 0
fi

# --- Refrescar icono en xfce4-panel ---
# --- 1) Extraer basename sin extensión ---
BASE_ON=$(basename "$ICON_ON" .png)
BASE_OFF=$(basename "$ICON_OFF" .png)

# --- 2) Detectar el plugin correcto en xfce4-panel ---
CHANNEL="xfce4-panel"
PROP_ROOT="/plugins"
PLUGIN_ID=""

for p in $(xfconf-query -c "$CHANNEL" -l | grep "^$PROP_ROOT/plugin-[0-9]\+/icon$"); do
	val=$(xfconf-query -c "$CHANNEL" -p "$p")
	if [[ "$val" == "$BASE_ON" || "$val" == "$BASE_OFF" ]]; then
		PLUGIN_ID=$(echo "$p" | sed -E 's#^/plugins/plugin-([0-9]+)/icon$#\1#')
		break
	fi
done

if [[ -z "$PLUGIN_ID" ]]; then
	echo "❌ No encontrado plugin con icono '$BASE_ON' o '$BASE_OFF'"
	return 1 2>/dev/null || exit 1
fi

PROP="/plugins/plugin-${PLUGIN_ID}/icon"

# --- 3) Forzar recarga del icono ---
old=$(xfconf-query -c "$CHANNEL" -p "$PROP")

# Icono temporal seguro
tmp="application-exit"
[[ "$old" == "$tmp" ]] && tmp="system-run"

xfconf-query -c "$CHANNEL" -p "$PROP" -s "$tmp"
xfconf-query -c "$CHANNEL" -p "$PROP" -s "$old"

# --- Fin del refresco ---
echo "✅ Icono de plugin-$PLUGIN_ID recargado (usando '$old')."

read -p "Presiona ENTER para cerrar..."[]
