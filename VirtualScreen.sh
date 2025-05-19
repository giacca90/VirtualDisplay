#!/bin/bash

# Parámetros
WIDTH=1920
HEIGHT=1080
RATE=60
OUTPUT="HDMI-1"
DESKTOP_FILE="$HOME/Escritorio/Proyector Virtual.desktop"

ICON_ON="/usr/share/icons/virtualScreenOn.png"
ICON_OFF="/usr/share/icons/virtualScreenOff.png"

log() {
    echo "[LOG] $1"
}

error_exit() {
    echo "[ERROR] $1"
    exit 1
}

# --- Comprobación de dependencias ---
REQUIRED_CMDS=(xrandr cvt cvlc)
MISSING=()
for cmd in "${REQUIRED_CMDS[@]}"; do
    if ! command -v "$cmd" &>/dev/null; then
        MISSING+=("$cmd")
    fi
done
if [ ${#MISSING[@]} -ne 0 ]; then
    error_exit "Faltan: ${MISSING[*]}. Instálalas y vuelve a ejecutar."
fi

# --- Detectar pantalla principal ---
PRIMARY_DISPLAY=$(xrandr | awk '/ connected primary/ {print $1; exit}')
if [ -z "$PRIMARY_DISPLAY" ]; then
    PRIMARY_DISPLAY=$(xrandr | awk '/ connected/ {print $1; exit}')
fi
log "Pantalla principal detectada: $PRIMARY_DISPLAY"

# --- Verificar salida HDMI-1 ---
if ! xrandr --query | grep -q "^$OUTPUT "; then
    error_exit "No se detecta la salida '$OUTPUT'. Conéctala y comprueba con xrandr."
fi
log "Salida $OUTPUT detectada"

# --- Comprobar o crear archivo .desktop ---
if [ ! -f "$DESKTOP_FILE" ]; then
    log "No existe $DESKTOP_FILE, creando uno básico..."
    cat > "$DESKTOP_FILE" <<EOF
[Desktop Entry]
Name=Proyector Virtual
Icon=$ICON_OFF
EOF
fi

# --- Función para matar VLC en puerto 2131 ---
kill_vlc() {
    log "Matando VLC en puerto 2131..."
    pkill -f ':2131/' 2>/dev/null
    pkill vlc 2>/dev/null
}

# --- Extraer modo correctamente ---
extract_mode() {
    local w=$1
    local h=$2
    local r=$3
    #log "Ejecutando cvt para ${w}x${h} @${r}Hz"
    
    # Ejecutar cvt y capturar salida real sin logs
    local cvt_output
    cvt_output=$(cvt "$w" "$h" "$r" 2>&1)
    if [ $? -ne 0 ]; then
        error_exit "Error ejecutando cvt: $cvt_output"
    fi

    # Extraer SOLO la línea que empieza con Modeline
    local modeline_line
    modeline_line=$(echo "$cvt_output" | grep -m1 '^Modeline')
    if [ -z "$modeline_line" ]; then
        error_exit "No se pudo extraer la línea Modeline de cvt"
    fi

    # Loguear la salida cvt excepto la línea Modeline
    #log "Salida cvt (sin Modeline):"
    #echo "$cvt_output" | grep -v '^Modeline' | while read -r line; do
    #    log "$line"
    #done

    # Extraer el nombre del modo sin comillas
    local mode_name
    mode_name=$(echo "$modeline_line" | sed -E 's/Modeline "([^"]+)".*/\1/')
    # log "Nombre del modo extraído: $mode_name"

    # Extraer los parámetros del modo (sin la palabra Modeline ni el nombre)
    local mode_params
    mode_params=$(echo "$modeline_line" | sed -E 's/Modeline "[^"]+" (.*)/\1/')

    # Devolver sólo la cadena que se usará en xrandr --newmode, sin ningún prefijo
    echo "$mode_name $mode_params"
}


MODELINE=$(extract_mode $WIDTH $HEIGHT $RATE)
MODE_NAME=$(echo "$MODELINE" | awk '{print $1}')

log "MODELINE extraído: $MODELINE"
log "MODE_NAME extraído: $MODE_NAME"

# --- Activar / Desactivar HDMI-1 ---
if xrandr --query | grep -q "^$OUTPUT connected [1-9][0-9]*x"; then
    log "Estado: HDMI-1 activo. Procediendo a apagarlo."
    kill_vlc

    log "Ejecutando: xrandr --output $OUTPUT --off"
    if ! xrandr --output $OUTPUT --off 2>/tmp/xrandr_off.err; then
        cat /tmp/xrandr_off.err
        error_exit "Fallo al apagar la salida $OUTPUT"
    fi

    log "Ejecutando: xrandr --delmode $OUTPUT $MODE_NAME"
    xrandr --delmode $OUTPUT $MODE_NAME 2>/tmp/xrandr_delmode.err
    cat /tmp/xrandr_delmode.err

    log "Ejecutando: xrandr --rmmode $MODE_NAME"
    xrandr --rmmode $MODE_NAME 2>/tmp/xrandr_rmmode.err
    cat /tmp/xrandr_rmmode.err

    log "Actualizando icono a OFF en $DESKTOP_FILE"
    sed -i "s|^Icon=.*|Icon=$ICON_OFF|" "$DESKTOP_FILE"
else
    log "Estado: HDMI-1 apagado. Procediendo a activarlo."

    log "Eliminando modo previo si existe"
    xrandr --delmode $OUTPUT $MODE_NAME 2>/tmp/xrandr_delmode.err
    cat /tmp/xrandr_delmode.err

    xrandr --rmmode $MODE_NAME 2>/tmp/xrandr_rmmode.err
    cat /tmp/xrandr_rmmode.err

    kill_vlc

    log "Ejecutando: xrandr --newmode $MODELINE"
    if ! xrandr --newmode $MODELINE 2>/tmp/xrandr_newmode.err; then
        cat /tmp/xrandr_newmode.err
        error_exit "Fallo al crear modo con xrandr --newmode"
    fi

    log "Ejecutando: xrandr --addmode $OUTPUT $MODE_NAME"
    if ! xrandr --addmode $OUTPUT $MODE_NAME 2>/tmp/xrandr_addmode.err; then
        cat /tmp/xrandr_addmode.err
        error_exit "Fallo al agregar modo con xrandr --addmode"
    fi

    log "Ejecutando: xrandr --output $OUTPUT --mode $MODE_NAME --above $PRIMARY_DISPLAY"
    if ! xrandr --output $OUTPUT --mode $MODE_NAME --above $PRIMARY_DISPLAY 2>/tmp/xrandr_output.err; then
        cat /tmp/xrandr_output.err
        error_exit "Fallo al activar salida con xrandr --output"
    fi

    sleep 1

    log "Iniciando VLC con streaming en puerto 2131"
    cvlc screen:// \
        :screen-fps=25 \
        :screen-left=$WIDTH \
        :screen-top=0 \
        :screen-width=$WIDTH \
        :screen-height=$HEIGHT \
        --sout '#transcode{vcodec=mp4v,vb=800,scale=1,acodec=none}:http{mux=ts,dst=:2131/}' &
    sleep 1

    log "Actualizando icono a ON en $DESKTOP_FILE"
    sed -i "s|^Icon=.*|Icon=$ICON_ON|" "$DESKTOP_FILE"
fi

# --- Refrescar XFCE ---
# log "Refrescando entorno XFCE..."
# killall xfsettingsd 2>/dev/null
# xfsettingsd &
# xfwm4 --replace &
# 
# log "Script finalizado."
