# Matar el Rato (MER) - Server

C server for the Matar el Rato game. Uses POSIX system calls and MySQL.

## Prerequisites
- Ubuntu/Linux (or WSL2)
- GCC & Make
- libmysqlclient-dev
- libcrypt-dev
- MySQL Server

## Deployment

### 1. Network
- **Router Port Forwarding**: Map TCP port 8888 to your local IP (e.g., 192.168.1.41).
- **Firewall (UFW)**: 
  ```bash
  sudo ufw allow 8888/tcp
  ```

### 2. Build
```bash
git pull
make
```

### 3. Run with Tmux
```bash
# Start session
tmux new -s tmux_mer

# Run server, default is 8888, but can be changed with a flag, no need to.
./server 
```

---

## Tmux Management

- **Detach**: Press `Ctrl + B`, then `D`. High-level exit, server keeps running.
- **Re-attach**: 
  ```bash
  tmux attach -t tmux_mer
  ```
- **Kill Server & Session**:
  - Inside session: `Ctrl + B`, then type `:kill-server` and press Enter.
  - Outside session: `tmux kill-session -t tmux_mer`

## Repository Files
- server.c: Request handling.
- db.c/h: Database layer.
- protocol.h: Binary protocol for C# clients.
- utils/: SQL schemas.
