#!/usr/bin/env python3
"""
Discord Drover GUI for Ubuntu / Linux
Matches the Windows version UI & functionality:
- HTTP / SOCKS5 / Direct modes
- Host, Port, Authentication (login, password)
- Install / Uninstall
- Works across Discord installations on Ubuntu (Flatpak, Snap, Native DEB/Tar)
"""

import os
import sys
import shutil
import subprocess
import configparser
import webbrowser
import tkinter as tk
from tkinter import ttk, messagebox

OPTIONS_FILENAME = "drover.ini"
PACKET_FILENAME = "drover-packet.bin"
LIB_FILENAME = "libdrover.so"

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

def get_possible_discord_dirs():
    """Return all detected Discord configuration/installation directories."""
    dirs = []
    home = os.path.expanduser("~")

    # 1. Flatpak config directory
    flatpak_dir = os.path.join(home, ".var/app/com.discordapp.Discord/config/discord")
    if os.path.exists(flatpak_dir):
        dirs.append(flatpak_dir)

    # 2. Native / Deb config directory
    native_dir = os.path.join(home, ".config/discord")
    if os.path.exists(native_dir):
        dirs.append(native_dir)

    # 3. Snap config directory
    snap_dir = os.path.join(home, "snap/discord/current/.config/discord")
    if os.path.exists(snap_dir):
        dirs.append(snap_dir)

    # Canary / PTB support
    for variant in ["discordcanary", "discordptb"]:
        p1 = os.path.join(home, f".config/{variant}")
        if os.path.exists(p1) and p1 not in dirs:
            dirs.append(p1)
        p2 = os.path.join(home, f".var/app/com.discordapp.{variant.capitalize()}/config/{variant}")
        if os.path.exists(p2) and p2 not in dirs:
            dirs.append(p2)

    # If no config dir exists yet, default to native ~/.config/discord and flatpak if installed
    if not dirs:
        dirs.append(native_dir)
        if os.path.exists(os.path.expanduser("~/.var/app/com.discordapp.Discord")):
            dirs.append(flatpak_dir)

    return dirs

def is_discord_running():
    """Check if any Discord process is currently running."""
    try:
        out = subprocess.check_output(["pgrep", "-i", "discord"], stderr=subprocess.DEVNULL)
        return len(out.strip()) > 0
    except subprocess.CalledProcessError:
        return False
    except Exception:
        return False

def parse_proxy_url(proxy_url):
    """Parse proxy URL into type, host, port, login, password."""
    data = {
        "type": "direct",
        "host": "",
        "port": "",
        "auth": False,
        "login": "",
        "password": ""
    }
    if not proxy_url:
        return data

    s = proxy_url.strip()
    if s.lower().startswith("socks5://"):
        data["type"] = "socks5"
        s = s[9:]
    elif s.lower().startswith("http://"):
        data["type"] = "http"
        s = s[7:]
    else:
        data["type"] = "http"

    if "@" in s:
        auth_part, rest = s.split("@", 1)
        data["auth"] = True
        if ":" in auth_part:
            data["login"], data["password"] = auth_part.split(":", 1)
        else:
            data["login"] = auth_part
        s = rest

    if ":" in s:
        host, port = s.split(":", 1)
        data["host"] = host
        data["port"] = port
    else:
        data["host"] = s
        data["port"] = "1080" if data["type"] == "socks5" else "8080"

    return data

def build_proxy_url(mode, host, port, is_auth, login, password):
    if mode == "direct":
        return ""
    proto = "socks5" if mode == "socks5" else "http"
    if is_auth and login:
        return f"{proto}://{login}:{password}@{host}:{port}"
    return f"{proto}://{host}:{port}"

def load_saved_options():
    dirs = get_possible_discord_dirs()
    for d in dirs:
        ini = os.path.join(d, OPTIONS_FILENAME)
        if os.path.isfile(ini):
            cp = configparser.ConfigParser()
            try:
                cp.read(ini)
                if "drover" in cp and "proxy" in cp["drover"]:
                    return cp["drover"]["proxy"]
            except Exception:
                pass
    local_ini = os.path.join(SCRIPT_DIR, OPTIONS_FILENAME)
    if os.path.isfile(local_ini):
        cp = configparser.ConfigParser()
        try:
            cp.read(local_ini)
            if "drover" in cp and "proxy" in cp["drover"]:
                return cp["drover"]["proxy"]
        except Exception:
            pass
    return ""

def configure_flatpak_flags(proxy_url, uninstall=False):
    """Add or remove proxy / LD_PRELOAD flags for flatpak Discord."""
    flatpak_config_dir = os.path.expanduser("~/.var/app/com.discordapp.Discord/config")
    flags_path = os.path.join(flatpak_config_dir, "discord-flags.conf")
    override_dir = os.path.expanduser("~/.local/share/flatpak/overrides")
    override_file = os.path.join(override_dir, "com.discordapp.Discord")

    if not os.path.exists(flatpak_config_dir) and not os.path.exists(os.path.expanduser("~/.var/app/com.discordapp.Discord")):
        return

    os.makedirs(flatpak_config_dir, exist_ok=True)
    os.makedirs(override_dir, exist_ok=True)

    # Update discord-flags.conf
    lines = []
    if os.path.exists(flags_path):
        with open(flags_path, "r") as f:
            lines = [l.strip() for l in f.readlines() if not l.strip().startswith("--proxy-server=")]

    if not uninstall and proxy_url:
        parsed = parse_proxy_url(proxy_url)
        # Chrome proxy-server flag doesn't support socks credentials directly; drover intercepts it
        proxy_flag = f"--proxy-server={parsed['type']}://{parsed['host']}:{parsed['port']}"
        lines.append(proxy_flag)

    with open(flags_path, "w") as f:
        f.write("\n".join(lines) + ("\n" if lines else ""))

    # Update Flatpak overrides to allow LD_PRELOAD
    lib_path = os.path.join(flatpak_config_dir, "discord", LIB_FILENAME)
    if uninstall:
        if os.path.exists(override_file):
            try:
                os.remove(override_file)
            except Exception:
                pass
    else:
        override_content = f"""[Environment]
LD_PRELOAD={lib_path}
ZYPAK_LD_PRELOAD={lib_path}
DROVER_DIR={os.path.join(flatpak_config_dir, 'discord')}
"""
        with open(override_file, "w") as f:
            f.write(override_content)

def configure_native_desktop(proxy_url, uninstall=False):
    """Create or update user desktop entry ~/.local/share/applications/discord.desktop to inject LD_PRELOAD."""
    apps_dir = os.path.expanduser("~/.local/share/applications")
    os.makedirs(apps_dir, exist_ok=True)
    desktop_file = os.path.join(apps_dir, "discord.desktop")

    if uninstall:
        if os.path.exists(desktop_file):
            try:
                os.remove(desktop_file)
            except Exception:
                pass
        return

    # Look for system desktop file
    sys_desktop = None
    for cand in ["/usr/share/applications/discord.desktop",
                 "/var/lib/flatpak/exports/share/applications/com.discordapp.Discord.desktop"]:
        if os.path.exists(cand):
            sys_desktop = cand
            break

    native_cfg_dir = os.path.expanduser("~/.config/discord")
    lib_path = os.path.join(native_cfg_dir, LIB_FILENAME)
    proxy_arg = ""
    if proxy_url:
        parsed = parse_proxy_url(proxy_url)
        proxy_arg = f" --proxy-server={parsed['type']}://{parsed['host']}:{parsed['port']}"

    content = f"""[Desktop Entry]
Name=Discord
StartupWMClass=discord
Comment=Discord with Drover proxy and voice chat bypass
GenericName=Internet Messenger
Exec=env LD_PRELOAD="{lib_path}" DROVER_DIR="{native_cfg_dir}" discord{proxy_arg} %U
Icon=discord
Type=Application
Categories=Network;InstantMessaging;
MimeType=x-scheme-handler/discord;
"""
    with open(desktop_file, "w") as f:
        f.write(content)

class DroverApp(tk.Tk):
    def __init__(self):
        super().__init__()

        self.title("Discord Drover")
        self.geometry("520x520")
        self.resizable(False, False)

        # Style & colors (delphi/windows neutral aesthetic)
        self.configure(bg="#f0f0f0")

        # Menu bar
        menubar = tk.Menu(self)
        self.config(menu=menubar)
        github_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="View on GitHub", command=self.open_github)

        self.proxy_type = tk.StringVar(value="direct")
        self.auth_enabled = tk.BooleanVar(value=False)
        self.host_var = tk.StringVar()
        self.port_var = tk.StringVar()
        self.login_var = tk.StringVar()
        self.pass_var = tk.StringVar()

        self.build_ui()
        self.load_initial_values()

    def open_github(self):
        webbrowser.open_new_tab("https://github.com/hdrover/discord-drover")

    def build_ui(self):
        # Radio button panel
        type_frame = tk.LabelFrame(self, text="Proxy Type", bg="#f0f0f0", font=("Segoe UI", 10, "bold"), padx=10, pady=5)
        type_frame.place(x=20, y=10, width=480, height=65)

        rb_http = tk.Radiobutton(type_frame, text="HTTP", variable=self.proxy_type, value="http",
                                 bg="#f0f0f0", font=("Segoe UI", 11), command=self.on_type_changed)
        rb_http.pack(side=tk.LEFT, padx=15)

        rb_socks = tk.Radiobutton(type_frame, text="SOCKS5", variable=self.proxy_type, value="socks5",
                                  bg="#f0f0f0", font=("Segoe UI", 11), command=self.on_type_changed)
        rb_socks.pack(side=tk.LEFT, padx=15)

        rb_direct = tk.Radiobutton(type_frame, text="Direct (Voice Bypass)", variable=self.proxy_type, value="direct",
                                   bg="#f0f0f0", font=("Segoe UI", 11), command=self.on_type_changed)
        rb_direct.pack(side=tk.LEFT, padx=15)

        # Host
        self.lbl_host = tk.Label(self, text="Host name:", bg="#f0f0f0", font=("Segoe UI", 11))
        self.lbl_host.place(x=25, y=95)
        self.entry_host = tk.Entry(self, textvariable=self.host_var, font=("Segoe UI", 11))
        self.entry_host.place(x=170, y=93, width=325, height=32)

        # Port
        self.lbl_port = tk.Label(self, text="Port number:", bg="#f0f0f0", font=("Segoe UI", 11))
        self.lbl_port.place(x=25, y=145)
        self.entry_port = tk.Entry(self, textvariable=self.port_var, font=("Segoe UI", 11))
        self.entry_port.place(x=170, y=143, width=150, height=32)

        # Authentication checkbox
        self.chk_auth = tk.Checkbutton(self, text="Authentication", variable=self.auth_enabled,
                                       bg="#f0f0f0", font=("Segoe UI", 11), command=self.on_auth_changed)
        self.chk_auth.place(x=25, y=195)

        # Login
        self.lbl_login = tk.Label(self, text="Login:", bg="#f0f0f0", font=("Segoe UI", 11))
        self.lbl_login.place(x=25, y=245)
        self.entry_login = tk.Entry(self, textvariable=self.login_var, font=("Segoe UI", 11))
        self.entry_login.place(x=170, y=243, width=325, height=32)

        # Password
        self.lbl_pass = tk.Label(self, text="Password:", bg="#f0f0f0", font=("Segoe UI", 11))
        self.lbl_pass.place(x=25, y=295)
        self.entry_pass = tk.Entry(self, textvariable=self.pass_var, show="•", font=("Segoe UI", 11))
        self.entry_pass.place(x=170, y=293, width=325, height=32)

        # Status note
        self.lbl_info = tk.Label(
            self,
            text="Direct mode bypasses Discord voice chat blocks without a proxy.\nSupports Flatpak, Snap, and Native Ubuntu Discord.",
            bg="#f0f0f0", fg="#666666", font=("Segoe UI", 9), justify=tk.LEFT
        )
        self.lbl_info.place(x=25, y=345)

        # Buttons
        self.btn_install = tk.Button(self, text="Install", bg="#0066cc", fg="white",
                                     font=("Segoe UI", 12, "bold"), command=self.on_install)
        self.btn_install.place(x=25, y=430, width=225, height=50)

        self.btn_uninstall = tk.Button(self, text="Uninstall", bg="#d9534f", fg="white",
                                       font=("Segoe UI", 12, "bold"), command=self.on_uninstall)
        self.btn_uninstall.place(x=270, y=430, width=225, height=50)

    def load_initial_values(self):
        saved = load_saved_options()
        parsed = parse_proxy_url(saved)
        self.proxy_type.set(parsed["type"])
        self.host_var.set(parsed["host"])
        self.port_var.set(parsed["port"])
        self.auth_enabled.set(parsed["auth"])
        self.login_var.set(parsed["login"])
        self.pass_var.set(parsed["password"])
        self.update_field_states()

    def on_type_changed(self):
        self.update_field_states()

    def on_auth_changed(self):
        self.update_field_states()

    def update_field_states(self):
        is_proxy = self.proxy_type.get() != "direct"
        auth_on = is_proxy and self.auth_enabled.get()

        proxy_state = tk.NORMAL if is_proxy else tk.DISABLED
        auth_state = tk.NORMAL if auth_on else tk.DISABLED

        self.lbl_host.config(state=proxy_state)
        self.entry_host.config(state=proxy_state)
        self.lbl_port.config(state=proxy_state)
        self.entry_port.config(state=proxy_state)
        self.chk_auth.config(state=proxy_state)

        self.lbl_login.config(state=auth_state)
        self.entry_login.config(state=auth_state)
        self.lbl_pass.config(state=auth_state)
        self.entry_pass.config(state=auth_state)

    def validate_inputs(self):
        mode = self.proxy_type.get()
        if mode == "direct":
            return True

        host = self.host_var.get().strip()
        port = self.port_var.get().strip()

        if not host:
            messagebox.showerror("Discord Drover", "Invalid host specified.")
            return False

        if not port.isdigit() or not (1 <= int(port) <= 65535):
            messagebox.showerror("Discord Drover", "Invalid port specified.")
            return False

        if self.auth_enabled.get():
            if mode == "socks5":
                messagebox.showerror(
                    "Discord Drover",
                    "Authentication for SOCKS5 is not supported in the current version. "
                    "Please use an unprotected proxy or switch to HTTP if authentication is required."
                )
                return False
            if not self.login_var.get().strip() or not self.pass_var.get().strip():
                messagebox.showerror("Discord Drover", "Fill in Login and Password or uncheck Authentication.")
                return False

        return True

    def check_running_discord(self):
        if is_discord_running():
            messagebox.showerror("Discord Drover", "Please exit Discord before proceeding.")
            return True
        return False

    def on_install(self):
        if not self.validate_inputs():
            return
        if self.check_running_discord():
            return

        mode = self.proxy_type.get()
        url = build_proxy_url(
            mode,
            self.host_var.get().strip(),
            self.port_var.get().strip(),
            self.auth_enabled.get(),
            self.login_var.get().strip(),
            self.pass_var.get().strip(),
        )

        lib_src = os.path.join(SCRIPT_DIR, LIB_FILENAME)
        packet_src = os.path.join(SCRIPT_DIR, PACKET_FILENAME)

        if not os.path.exists(lib_src):
            messagebox.showerror("Discord Drover", f"Missing binary file: {LIB_FILENAME}")
            return

        discord_dirs = get_possible_discord_dirs()
        errors = []

        # Write ini content
        ini_content = f"[drover]\nproxy = {url}\n"

        # Also save in SCRIPT_DIR
        try:
            with open(os.path.join(SCRIPT_DIR, OPTIONS_FILENAME), "w") as f:
                f.write(ini_content)
        except Exception as e:
            errors.append(f"{SCRIPT_DIR}: {e}")

        # Install into each detected discord config dir
        for d in discord_dirs:
            try:
                os.makedirs(d, exist_ok=True)
                with open(os.path.join(d, OPTIONS_FILENAME), "w") as f:
                    f.write(ini_content)
                shutil.copy2(lib_src, os.path.join(d, LIB_FILENAME))
                if os.path.exists(packet_src):
                    shutil.copy2(packet_src, os.path.join(d, PACKET_FILENAME))
            except Exception as e:
                errors.append(f"{d}: {e}")

        # Configure Flatpak environment & flags
        try:
            configure_flatpak_flags(url, uninstall=False)
        except Exception as e:
            errors.append(f"Flatpak config: {e}")

        # Configure desktop launcher for system/native Discord
        try:
            configure_native_desktop(url, uninstall=False)
        except Exception as e:
            errors.append(f"Desktop launcher: {e}")

        if errors:
            messagebox.showerror("Discord Drover - Installation Error", "Some errors occurred:\n" + "\n".join(errors))
        else:
            messagebox.showinfo("Discord Drover", "Installation complete!\n\nYou can now launch Discord.")

    def on_uninstall(self):
        if self.check_running_discord():
            return

        discord_dirs = get_possible_discord_dirs()
        errors = []

        for d in discord_dirs:
            for fname in [OPTIONS_FILENAME, LIB_FILENAME, PACKET_FILENAME]:
                fpath = os.path.join(d, fname)
                if os.path.exists(fpath):
                    try:
                        os.remove(fpath)
                    except Exception as e:
                        errors.append(f"{fpath}: {e}")

        try:
            configure_flatpak_flags("", uninstall=True)
        except Exception as e:
            errors.append(f"Flatpak cleanup: {e}")

        try:
            configure_native_desktop("", uninstall=True)
        except Exception as e:
            errors.append(f"Desktop launcher cleanup: {e}")

        if errors:
            messagebox.showerror("Discord Drover - Uninstallation Error", "Some files could not be deleted:\n" + "\n".join(errors))
        else:
            messagebox.showinfo("Discord Drover", "Uninstallation complete. All files have been successfully removed.")

if __name__ == "__main__":
    app = DroverApp()
    app.mainloop()
