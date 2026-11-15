# Ubuntu mínima con Agente IA (o cualquier proyecto Python) como demonio

`rehacer-ubuntu.sh` rehace la ISO de Ubuntu Server para que la instalación mínima lleve el proyecto preinstalado como
servicio systemd activo y preconfigurado.

```bash
sudo apt install xorriso squashfs-tools
sudo ./rehacer-ubuntu.sh --descargar 24.04 --deb nexus-office_4.2.0_amd64.deb --probar
sudo ./rehacer-ubuntu.sh --ayuda
```

Documentación completa en [docs/UBUNTU.md](../../docs/UBUNTU.md).
