#!/usr/bin/env bash
set -euo pipefail

VKMS_VER="1.0"
KERNEL_VER="$(uname -r)"
DKMS_SRC="/usr/src/vkms-${VKMS_VER}"
NJOBS="$(nproc)"

echo "==> 1. Instalar dependencias y headers del kernel actual"
sudo apt update
sudo apt install -y build-essential dkms linux-headers-"${KERNEL_VER}" \
	libncurses-dev bison flex libssl-dev libelf-dev bc apt-src

echo
echo "==> 2. Descargar fuentes del kernel"
cd "$HOME"
apt source linux-image-"${KERNEL_VER}"
KERN_SRC_DIR="$(find . -maxdepth 1 -type d -name 'linux-*' | head -n1)"
if [ -z "$KERN_SRC_DIR" ]; then
	echo "ERROR: No se encontró el directorio de fuentes del kernel"
	exit 1
fi

cd "$KERN_SRC_DIR"

echo
echo "==> 3. Configurar VKMS en .config del kernel"
grep -q '^CONFIG_DRM_VKMS=m' .config || {
	echo "-> Activando CONFIG_DRM_VKMS=m"
	sed -i 's/^# \(CONFIG_DRM_VKMS\)=.*/\1=m/' .config || echo 'CONFIG_DRM_VKMS=m' >>.config
	make olddefconfig
}

echo
echo "==> 4. Compilar vkms.ko"
make -j"${NJOBS}" drivers/gpu/drm/vkms/
if [ ! -f drivers/gpu/drm/vkms/vkms.ko ]; then
	echo "ERROR: vkms.ko no se generó correctamente"
	exit 1
fi

echo
echo "==> 5. Preparar DKMS"
sudo rm -rf "${DKMS_SRC}"
sudo mkdir -p "${DKMS_SRC}"
cd drivers/gpu/drm/vkms/
sudo cp *.c *.h Makefile "${DKMS_SRC}/"

sudo tee "${DKMS_SRC}/Makefile" >/dev/null <<'EOF'
obj-m += vkms.o
vkms-objs := vkms_drv.o vkms_output.o vkms_plane.o vkms_crtc.o vkms_composer.o vkms_formats.o vkms_writeback.o
EOF

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
echo "==> 6. Registrar e instalar con DKMS"
sudo dkms remove -m vkms -v "${VKMS_VER}" --all || true
sudo dkms add -m vkms -v "${VKMS_VER}"
sudo dkms build -m vkms -v "${VKMS_VER}"
sudo dkms install -m vkms -v "${VKMS_VER}"

echo
echo "==> 7. Cargar módulo vkms"
sudo modprobe vkms

if lsmod | grep -q '^vkms'; then
	echo "✅ vkms cargado correctamente"
else
	echo "⚠️ vkms no se cargó. Ejecuta: sudo modprobe vkms"
fi

echo
echo "==> 8. Crear ~/.xprofile si no existe"
XPROFILE="$HOME/.xprofile"
if [ ! -f "$XPROFILE" ]; then
	cat >"$XPROFILE" <<'EOF'
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

	chmod +x "$XPROFILE"
	echo "📝 ~/.xprofile creado correctamente"
else
	echo "ℹ️ ~/.xprofile ya existe, no se sobrescribió"
fi

echo
echo "==> 9. Crear y habilitar servicio systemd para recompilar vkms tras actualizaciones del kernel"

SERVICE_PATH="/etc/systemd/system/vkms-autoinstall.service"

sudo tee "${SERVICE_PATH}" >/dev/null <<'EOF'
[Unit]
Description=Auto-reinstala el módulo vkms con DKMS tras actualizaciones del kernel
After=dkms.service
ConditionPathExists=/usr/src/vkms-1.0

[Service]
Type=oneshot
ExecStart=/usr/sbin/dkms autoinstall

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable vkms-autoinstall.service

echo "✅ Servicio vkms-autoinstall creado y habilitado"

echo
echo "🎉 ¡Script completado! vkms instalado, módulo cargado, y servicio systemd activo."
