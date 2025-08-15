```
 _________________________________________________________________________________
|                                                                                 |
|  GGGGGG    IIII     AAA      CCCCCC    CCCCCC      AAA      9999999     00000   |
| GG    GG    II     AA AA    CC    CC  CC    CC    AA AA    99     99   00   00  |
| GG          II    AA   AA   CC        CC         AA   AA   99     99  00     00 |
| GG   GGGG   II   AA     AA  CC        CC        AA     AA   99999999  00     00 |
| GG    GG    II   AAAAAAAAA  CC        CC        AAAAAAAAA         99  00     00 |
| GG    GG    II   AA     AA  CC    CC  CC    CC  AA     AA  99     99   00   00  |
|  GGGGGG    IIII  AA     AA   CCCCCC    CCCCCC   AA     AA   9999999     00000   |
|_________________________________________________________________________________|
```

# virtualScreen

## ¿Qué es?

virtualScreen es un pequeño programa que permite a los usuarios ver sus pantallas virtuales en un navegador web.

## ¿Cómo funciona?

virtualScreen es un servidor web que sirve como una interfaz para los usuarios que quieran ver sus pantallas virtuales en un navegador web. Utiliza GStreamer para capturar la pantalla y enviarla por WebRTC a los usuarios.

### Requisitos

Esta la cree para mi uso, en Kali Linux con XFCE. Funciona SOLO con X11 (no funciona en Wayland). No puedo garantizar que funcione en otras distribuciones o entornos distintos a XFCE. Yo lo utilizo con VKMS, para crear una segunda pantalla virtual, pero no es necesario si solo quieres emitir la pantalla a otro navegador web.

## ¿Por qué?

Porque tengo un proyector que uso como segunda pantalla, y no quiería cables de por medio.
