#!/usr/bin/env bash
set -euo pipefail

VKMS_VER="1.0"
KERNEL_VER="$(uname -r)"
# Quitar sufijos como +kali o -amd64 para obtener la versión base
KERNEL_BASE="$(echo "$KERNEL_VER" | sed -E 's/[+].*//; s/-.*//')"  # e.g. 6.16.8
KERNEL_SHORT="$(echo "$KERNEL_BASE" | cut -d. -f1,2)"              # e.g. 6.16
DKMS_SRC="/usr/src/vkms-${VKMS_VER}"

echo "==> 1. Instalar dependencias y headers del kernel actual"
sudo apt update -y
sudo apt install -y build-essential dkms linux-headers-"${KERNEL_VER}" libelf-dev wget xz-utils ca-certificates rsync

echo
echo "==> 2. Obtener código fuente del kernel actual (${KERNEL_BASE})"
cd "$HOME"
SRC_DIR="$(find . -maxdepth 1 -type d -name "linux-source-${KERNEL_SHORT}*" | head -n1 || true)"

if [ -z "$SRC_DIR" ]; then
  echo "   - Intentando apt source linux-source-${KERNEL_SHORT}..."
  apt source linux-source-"${KERNEL_SHORT}" >/dev/null 2>&1 || true
  SRC_DIR="$(find . -maxdepth 1 -type d -name "linux-source-${KERNEL_SHORT}*" | head -n1 || true)"
fi

if [ -z "$SRC_DIR" ]; then
  echo "   - No disponible vía APT. Descargando de kernel.org..."
  KERNEL_MAJOR="$(echo "$KERNEL_BASE" | cut -d. -f1)"
  KERNEL_TARBALL="linux-${KERNEL_BASE}.tar.xz"
  KERNEL_URL="https://cdn.kernel.org/pub/linux/kernel/v${KERNEL_MAJOR}.x/${KERNEL_TARBALL}"

  echo "   - Descargando ${KERNEL_URL}"
  wget -q "$KERNEL_URL" -O linux.tar.xz || {
    echo "❌ No se pudo descargar el código fuente del kernel ${KERNEL_BASE}"
    exit 1
  }

  echo "   - Extrayendo el código fuente..."
  tar -xf linux.tar.xz
  rm linux.tar.xz

  # Detectar directorio extraído (seguro)
  EXTRACTED_DIR="$(find . -maxdepth 1 -type d -name "linux-${KERNEL_BASE}" | head -n1 || true)"
  if [ -z "$EXTRACTED_DIR" ]; then
    EXTRACTED_DIR="$(find . -maxdepth 1 -type d -name "linux-[0-9]*" | sort -r | head -n1 || true)"
  fi

  if [ -n "$EXTRACTED_DIR" ]; then
    mv "$EXTRACTED_DIR" "linux-source-${KERNEL_SHORT}"
    SRC_DIR="$HOME/linux-source-${KERNEL_SHORT}"
  else
    echo "❌ No se pudo identificar el directorio extraído del kernel."
    exit 1
  fi
fi

if [ ! -d "$SRC_DIR/drivers/gpu/drm/vkms" ]; then
  echo "❌ No se encontró el código fuente de vkms en $SRC_DIR"
  exit 1
fi
echo "   - Fuente encontrada en: $SRC_DIR"

echo
echo "==> 3. Preparar carpeta para DKMS"
# limpiar y recrear /usr/src/vkms-1.0 (con permisos root)
sudo rm -rf "${DKMS_SRC}"
sudo mkdir -p "${DKMS_SRC}"

# copiar el árbol completo de vkms respetando la estructura, excluyendo tests/
sudo rsync -a --delete --exclude='tests' "$SRC_DIR/drivers/gpu/drm/vkms/" "${DKMS_SRC}/"

# Generar Makefile dinámico: incluye todos los .c (con rutas relativas) excepto tests
echo "obj-m += vkms.o" | sudo tee "${DKMS_SRC}/Makefile" >/dev/null

# Crear la lista de objetos a partir de los .c detectados
OBJS="$(cd "${DKMS_SRC}" && find . -type f -name '*.c' ! -path './tests/*' -printf '%P\n' | sed 's/\.c$/.o/' | tr '\n' ' ' | sed 's/ $//')"

if [ -z "$OBJS" ]; then
  echo "❌ No se detectaron fuentes .c dentro de ${DKMS_SRC}"
  exit 1
fi

echo "vkms-objs := ${OBJS}" | sudo tee -a "${DKMS_SRC}/Makefile" >/dev/null

echo
echo "   - Archivos incluidos en Makefile:"
cd "${DKMS_SRC}" && find . -type f -name '*.c' ! -path './tests/*' -printf '      • %P\n'

# Crear dkms.conf con ruta /build (importante: DKMS copiará a .../build)
sudo tee "${DKMS_SRC}/dkms.conf" >/dev/null <<EOF
PACKAGE_NAME="vkms"
PACKAGE_VERSION="${VKMS_VER}"
MAKE="make -C /lib/modules/${KERNEL_VER}/build M=\$dkms_tree/\$PACKAGE_NAME/\$PACKAGE_VERSION/build modules"
CLEAN="make -C /lib/modules/${KERNEL_VER}/build M=\$dkms_tree/\$PACKAGE_NAME/\$PACKAGE_VERSION/build clean"
BUILT_MODULE_NAME[0]="vkms"
DEST_MODULE_LOCATION[0]="/kernel/drivers/gpu/drm/vkms"
AUTOINSTALL="yes"
EOF

echo
echo "==> 4. Registrar e instalar con DKMS"
# remover versiones previas (si existen)
sudo dkms remove -m vkms -v "${VKMS_VER}" --all || true
sudo dkms add -m vkms -v "${VKMS_VER}"
sudo dkms build -m vkms -v "${VKMS_VER}"
sudo dkms install -m vkms -v "${VKMS_VER}"

echo
echo "==> 5. Cargar módulo vkms"
sudo modprobe vkms || true

if lsmod | grep -q '^vkms'; then
  echo "✅ vkms cargado correctamente"
else
  echo "⚠️ vkms no se cargó. Ejecuta: sudo modprobe vkms"
fi
