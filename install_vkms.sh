#!/usr/bin/env bash
set -euo pipefail

# Nombre de la versión DKMS
VKMS_VER="1.0"
DKMS_SRC="/usr/src/vkms-${VKMS_VER}"
NJOBS="$(nproc)"

echo "==> 1. Instalar dependencias necesarias"
sudo apt update
sudo apt install -y build-essential libncurses-dev bison flex libssl-dev libelf-dev bc dkms apt-src

echo
echo "==> 2. Obtener fuentes del kernel"
cd "$HOME"
apt source linux-image-"$(uname -r)"
# Busca el directorio creado (puede variar un poco)
KERN_SRC_DIR="$(find . -maxdepth 1 -type d -name 'linux-*' | head -n1)"
if [ -z "$KERN_SRC_DIR" ]; then
	echo "ERROR: No se encontró el directorio de fuentes del kernel"
	exit 1
fi
cd "$KERN_SRC_DIR"

echo
echo "==> 3. Configurar VKMS en el kernel"
# Asegúrate de que CONFIG_DRM_VKMS=m; si no, lo parcheamos
grep -q '^CONFIG_DRM_VKMS=m' .config || {
	echo "-> Parcheando .config para habilitar vkms como módulo"
	sed -i 's/^# \(CONFIG_DRM_VKMS\)=.*/\1=m/' .config ||
		echo 'CONFIG_DRM_VKMS=m' >>.config
	make olddefconfig
}

echo
echo "==> 4. Compilar sólo vkms"
make -j"${NJOBS}" drivers/gpu/drm/vkms/

# Comprueba que existe
if [ ! -f drivers/gpu/drm/vkms/vkms.ko ]; then
	echo "ERROR: vkms.ko no se generó" >&2
	exit 1
fi

echo
echo "==> 5. Instalar módulo manualmente (primera vez)"
sudo mkdir -p /lib/modules/"$(uname -r)"/kernel/drivers/gpu/drm/vkms/
sudo cp drivers/gpu/drm/vkms/vkms.ko \
	/lib/modules/"$(uname -r)"/kernel/drivers/gpu/drm/vkms/
sudo depmod -a
sudo modprobe vkms

echo
echo "==> 6. Preparar DKMS"
# Limpia cualquier instalación previa
sudo dkms remove -m vkms -v "${VKMS_VER}" --all || true

sudo mkdir -p "${DKMS_SRC}"
cd drivers/gpu/drm/vkms/
sudo cp *.c *.h Makefile "${DKMS_SRC}/"
# Crear Makefile DKMS
sudo tee "${DKMS_SRC}/Makefile" >/dev/null <<'EOF'
obj-m += vkms.o

vkms-objs := vkms_drv.o vkms_output.o vkms_plane.o vkms_crtc.o vkms_composer.o vkms_formats.o vkms_writeback.o
EOF

# Crear dkms.conf
sudo tee "${DKMS_SRC}/dkms.conf" >/dev/null <<EOF
PACKAGE_NAME="vkms"
PACKAGE_VERSION="${VKMS_VER}"
MAKE="make -C \$kernel_source_dir M=\$dkms_tree/\$PACKAGE_NAME/\$PACKAGE_VERSION/build modules"
CLEAN="make -C \$kernel_source_dir M=\$dkms_tree/\$PACKAGE_NAME/\$PACKAGE_VERSION/build clean"
BUILT_MODULE_NAME[0]="vkms"
DEST_MODULE_LOCATION[0]="/kernel/drivers/gpu/drm/vkms"
AUTOINSTALL="yes"
EOF

echo
echo "==> 7. Registrar y compilar con DKMS"
sudo dkms add -m vkms -v "${VKMS_VER}"
sudo dkms build -m vkms -v "${VKMS_VER}"
sudo dkms install -m vkms -v "${VKMS_VER}"

echo
echo "==> 8. Comprobación final"
dkms status | grep "^vkms, ${VKMS_VER}"
if lsmod | grep -q '^vkms'; then
	echo "👑 vkms cargado correctamente"
else
	echo "⚠️ vkms NO está cargado, ejecuta: sudo modprobe vkms"
fi

echo
echo "==> 9. Crear ~/.xprofile para configurar pantalla virtual arriba de la principal"

cat >"$HOME/.xprofile" <<'EOF'
#!/bin/bash

# Esperar a que xrandr esté listo
for i in {1..10}; do
    xrandr | grep -q " connected" && break
    sleep 1
done

PRIMARY=$(xrandr --listmonitors | awk '$2 == "*" {print $4}')
if [ -z "$PRIMARY" ]; then
    echo "No se detectó pantalla principal."
    exit 1
fi

RESOLUTION=$(xrandr | awk -v output="$PRIMARY" '
  $1 == output && $2 == "connected" {
    getline
    if ($2 == "*") print $1
  }
')
if [ -z "$RESOLUTION" ]; then
    echo "No se pudo obtener resolución de la pantalla principal."
    exit 1
fi

GEOM=$(xrandr | grep -w "$PRIMARY" | grep -oP '[0-9]+x[0-9]+')
WIDTH=$(echo "$GEOM" | cut -dx -f1)
HEIGHT=$(echo "$GEOM" | cut -dx -f2)
VIRTUAL_SIZE="${WIDTH}x${HEIGHT}"
TOTAL_HEIGHT=$((HEIGHT * 2))

xrandr --fb ${WIDTH}x${TOTAL_HEIGHT}
xrandr --setmonitor Virtual-1-1 ${VIRTUAL_SIZE}+0+${HEIGHT} none
xrandr --output Virtual-1-1 --mode $RESOLUTION --above "$PRIMARY"
xrandr --output "$PRIMARY" --primary
EOF

chmod +x "$HOME/.xprofile"

echo
echo "✅ ¡Script completado y ~/.xprofile creado para configurar pantalla virtual al iniciar sesión!"
