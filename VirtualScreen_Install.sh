#!/bin/bash
# Instalador de VirtualScreen

PLUGIN_DIR="/usr/share/virtualScreen"
SRC_DIR="$(realpath "$(dirname "$0")/virtualScreen")"
DESKTOP_FILE="$PLUGIN_DIR/Proyector Virtual.desktop"
USER_DESKTOP="$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Escritorio")"
SYMLINK="$USER_DESKTOP/Proyector Virtual.desktop"
TOGGLE_SCRIPT="$PLUGIN_DIR/VirtualScreenToggle.sh"
ICON_OFF="/usr/share/icons/virtualScreenOff.png"

log() { echo "[LOG] $1"; }
error_exit() {
	echo "[ERROR] $1"
	exit 1
}

# --- Función para ejecutar comandos con privilegios gráficos ---
run_as_root() {
	if command -v pkexec &>/dev/null; then
		pkexec bash "$@"
	elif command -v gksudo &>/dev/null; then
		gksudo bash "$@"
	elif command -v kdesudo &>/dev/null; then
		kdesudo bash "$@"
	else
		echo "No se encontró pkexec, gksudo ni kdesudo. Ejecuta este script con sudo." >&2
		exit 1
	fi
}

log "=== Instalando VirtualScreen ==="

# Un solo bloque root para todo lo que requiere permisos
ROOT_SCRIPT=$(mktemp)
cat >"$ROOT_SCRIPT" <<EOF
if [ -d '$PLUGIN_DIR' ]; then
	rm -rf '$PLUGIN_DIR' || exit 1
fi
mkdir -p '$PLUGIN_DIR' || exit 1
if [ -d '$SRC_DIR' ] && [ "\$(ls -A '$SRC_DIR')" ]; then
	cp -r '$SRC_DIR/'* '$PLUGIN_DIR/' || exit 1
else
	echo 'No se encontró la carpeta $SRC_DIR o está vacía' >&2
	exit 1
fi
chmod -R a+rwX '$PLUGIN_DIR'
chmod a+rwx '$PLUGIN_DIR/VirtualScreenToggle.sh'
EOF

run_as_root "$ROOT_SCRIPT"
rm -f "$ROOT_SCRIPT"

# Crear .desktop en la carpeta del programa (no requiere root)
cat >"$DESKTOP_FILE" <<EODESK
[Desktop Entry]
Name=Proyector Virtual
Comment=Activa o desactiva el servidor virtual
Exec=$TOGGLE_SCRIPT
Icon=$ICON_OFF
Terminal=true
Type=Application
Categories=Utility;
EODESK
chmod a+rwx "$DESKTOP_FILE"
log "Acceso directo creado: $DESKTOP_FILE"

# Crear enlace simbólico en el escritorio del usuario (no requiere root)
if [ -L "$SYMLINK" ] || [ -e "$SYMLINK" ]; then
	rm -f "$SYMLINK"
fi
ln -s "$DESKTOP_FILE" "$SYMLINK"
log "Enlace simbólico creado en el escritorio."

log "=== Instalación completada ==="
