# Discord Drover for Linux

An open-source utility for Linux systems providing process-level network desynchronization and proxy tunneling for Discord and compatible clients (such as Vesktop).

## Core Capabilities

- **Direct Voice VoIP Desync:** Intercepts outbound STUN handshake traffic on UDP sockets and injects state desynchronization packets before delivering the authentic handshake, bypassing regional VoIP traffic blocking without requiring a full VPN.
- **Proxy Support:** Supports standard HTTP and SOCKS5 proxy configurations for client connections.
- **Native & Sandboxed Clients:** Works with native Discord installations, client wrappers, and compatible alternatives.
- **Cross-Platform Parity:** Offers both an interactive graphical interface and command-line automation.

## Architecture

On Linux systems, the dynamic linker (`LD_PRELOAD`) allows intercepting socket API calls:
- Intercepts `send`, `sendto`, and `sendmsg` system calls.
- Identifies STUN binding requests matching protocol headers.
- Emits initial desynchronization frames followed by a brief timing pause before releasing authentic data.

## Installation

### Prerequisites (Debian / Ubuntu)

```bash
sudo apt update && sudo apt install -y build-essential python3 python3-tk
```

### Build & Install

Run the automated installer:

```bash
git clone https://github.com/ashrymahrm/discord-drover-linux.git
cd discord-drover-linux
./install.sh
```

### Graphical Interface

To open the graphical configuration utility:

```bash
./drover
```

### Manual Run

To launch Discord with desynchronization enabled directly:

```bash
env LD_PRELOAD="./libdrover.so" discord &
```

### Uninstallation

```bash
./uninstall.sh
```

## License

MIT License. See [LICENSE](LICENSE).
