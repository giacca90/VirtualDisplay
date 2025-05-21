#!/bin/bash

# Parámetros de la pantalla virtual
WIDTH=1920
HEIGHT=1080
VIRTUAL_NAME="VIRTUAL1"
OUTPUT="HDMI-1"

# Verificar si el controlador dummy ya está cargado
if ! lsmod | grep -q "dummy"; then
	echo "Cargando el módulo dummy..."
	sudo modprobe dummy
fi

# Crear la pantalla virtual
echo "Creando la pantalla virtual $VIRTUAL_NAME..."
xrandr --setmonitor $VIRTUAL_NAME ${WIDTH}x${HEIGHT}+0+0 none

# Activar la pantalla virtual
echo "Activando la pantalla virtual..."
xrandr --output $VIRTUAL_NAME --mode ${WIDTH}x${HEIGHT} --above $OUTPUT

echo "Pantalla virtual $VIRTUAL_NAME activada."
