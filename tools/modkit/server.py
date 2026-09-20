"""Compatibility launcher. Melee Workshop now runs as an Electron desktop app."""
from pathlib import Path
import subprocess
if __name__ == '__main__':
 root=Path(__file__).resolve().parents[2]
 raise SystemExit(subprocess.call(['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',str(root/'scripts/run_workshop.ps1')],cwd=root))
