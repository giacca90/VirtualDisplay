#!/bin/bash

# Parámetros
# El virtual será igual que la principal, así que aquí lo dejamos vacío para asignar luego
VIRTUAL_NAME="VIRTUAL1"
DESKTOP_FILE="$HOME/Escritorio/Proyector Virtual.desktop"
ICON_ON="/usr/share/icons/virtualScreenOn.png"
ICON_OFF="/usr/share/icons/virtualScreenOff.png"

log() { echo "[LOG] $1"; }
error_exit() {
	echo "[ERROR] $1"
	exit 1
}

log "=== Arrancando VirtualScreen.sh ==="

# --- Comprobación de dependencias ---
REQUIRED_CMDS=(xrandr awk sed)
MISSING=()
for cmd in "${REQUIRED_CMDS[@]}"; do
	if ! command -v "$cmd" &>/dev/null; then
		MISSING+=("$cmd")
	fi
done
[ ${#MISSING[@]} -ne 0 ] && error_exit "Faltan: ${MISSING[*]}. Instálalas y vuelve a ejecutar."
log "Dependencias OK."

# --- Detectar pantalla principal ---
log "Detectando pantalla principal..."
PRIMARY=$(xrandr | awk '/ connected primary/ {print $1; exit}')
[ -z "$PRIMARY" ] && PRIMARY=$(xrandr | awk '/ connected/ {print $1; exit}')
[ -z "$PRIMARY" ] && error_exit "No se detectó ninguna pantalla conectada."
log "Pantalla principal: $PRIMARY"

# --- Obtener resolución y tamaño físico de la principal ---
log "Obteniendo resolución de $PRIMARY..."
RES=$(xrandr | grep "^$PRIMARY connected" | grep -oP '[0-9]+x[0-9]+' | head -1)
[ -z "$RES" ] && error_exit "No se pudo determinar la resolución de $PRIMARY."
WIDTH=${RES%x*}
HEIGHT=${RES#*x}
log "Resolución principal: ${WIDTH}x${HEIGHT}"

log "Obteniendo dimensiones físicas de $PRIMARY..."
MM=$(xrandr | grep "^$PRIMARY connected" | sed -En 's/.* ([0-9]+)mm x ([0-9]+)mm.*/\1 \2/p')
[ -z "$MM" ] && error_exit "No se pudo leer dimensiones físicas de $PRIMARY."
PX=$(echo "$MM" | awk '{print $1}')
PY=$(echo "$MM" | awk '{print $2}')
log "Tamaño físico: ${PX}mm x ${PY}mm"

# --- Calcular dimensiones físicas de la pantalla virtual (igual a la principal) ---
VIRTUAL_WIDTH=$WIDTH
VIRTUAL_HEIGHT=$HEIGHT
VMMW=$PX
VMMH=$PY
log "Dimensiones físicas virtual (igual a principal): ${VMMW}mm x ${VMMH}mm"

# --- Crear o comprobar .desktop ---
if [ ! -f "$DESKTOP_FILE" ]; then
	log "No existe $DESKTOP_FILE, creando..."
	cat >"$DESKTOP_FILE" <<EOF
[Desktop Entry]
Name=Proyector Virtual
Comment=Activa o desactiva la pantalla virtual
Exec=$0
Icon=$ICON_OFF
Terminal=false
Type=Application
Categories=Utility;
EOF
	log ".desktop creado."
fi

# --- Función para saber si existe ---
virtual_exists() {
	xrandr --listmonitors | grep -q "$VIRTUAL_NAME"
}

# --- Activar / Desactivar ---
if virtual_exists; then
	log "Monitor virtual activo. Eliminando..."
	log "Ejecutando: xrandr --delmonitor $VIRTUAL_NAME"
	xrandr --delmonitor "$VIRTUAL_NAME" || error_exit "Fallo al eliminar $VIRTUAL_NAME"
	log "Restableciendo framebuffer a ${WIDTH}x${HEIGHT}..."
	xrandr --fb "${WIDTH}x${HEIGHT}" || error_exit "Fallo al ajustar framebuffer a ${WIDTH}x${HEIGHT}"
	log "Monitor virtual eliminado."
	log "Actualizando icono a OFF"
	sed -i "s|^Icon=.*|Icon=$ICON_OFF|" "$DESKTOP_FILE"
	log "Icono OFF aplicado."
else
	log "Monitor virtual inactivo. Creando..."
	GEOMETRY="${VIRTUAL_WIDTH}/${VMMW}x${VIRTUAL_HEIGHT}/${VMMH}+0+${HEIGHT}"
	log "Ejecutando: xrandr --setmonitor $VIRTUAL_NAME $GEOMETRY none"
	xrandr --setmonitor "$VIRTUAL_NAME" "$GEOMETRY" none ||
		error_exit "Fallo al crear monitor virtual con '$GEOMETRY'"
	log "Monitor virtual creado."
	log "Ajustando framebuffer para apilar verticalmente..."
	FB_WIDTH=$WIDTH
	FB_HEIGHT=$((HEIGHT * 2))
	log "Ejecutando: xrandr --fb ${FB_WIDTH}x${FB_HEIGHT}"
	xrandr --fb "${FB_WIDTH}x${FB_HEIGHT}" ||
		error_exit "Fallo al ajustar framebuffer a ${FB_WIDTH}x${FB_HEIGHT}"
	log "Framebuffer ajustado."
	log "Actualizando icono a ON"
	sed -i "s|^Icon=.*|Icon=$ICON_ON|" "$DESKTOP_FILE"
	log "Icono ON aplicado."
fi

log "=== Script finalizado correctamente ==="
