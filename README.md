# GitImg

A custom, high-performance version control system tailored for artists and binary asset management. GitImg leverages FastCDC (Content-Defined Chunking) and FNV-1a hashing to provide efficient deduplication and storage for large binary files over your local network.

## Architecture

GitImg is built on pure C++17 with zero external dependencies, utilizing POSIX/Linux native APIs for low-level system interaction.

* **Client (gitimg)**: Performs client-side chunking and pushes only unique data to the server. Now supports LAN-based discovery and persistent remote configuration.
* **Server (gitimgd)**: A multi-client TCP socket daemon that handles append-only packfile storage, session-based authentication, and dynamic image reconstruction.
* **Web Interface**: An embedded HTTP dashboard that dynamically renders your repository's gallery and commit history in a dark-mode, GitHub-inspired interface.

## Core Features

* **LAN-Ready**: Server configuration allows binding to `0.0.0.0`, enabling multi-device access across your home network.
* **Network Deduplication**: Only new or changed binary chunks are pushed to the server, maximizing performance over Wi-Fi.
* **Identity & Security**: Integrated user registration, login, and session-based token authentication with 403 Forbidden access control.
* **Zero-Config Workflow**: Automatic repository creation and asset mapping on the first `push`.
* **Responsive Web UI**: Built-in gallery grid that works on mobile, tablet, and desktop browsers.

## Quick Start

### 1. Compile

    make clean && make

### 2. Configure and Run Server

Create a `server.conf` file:

    BIND_HOST=0.0.0.0
    PORT=8080
    STORAGE_DIR=.gitimgd

Run the daemon:

    ./gitimgd server.conf

### 3. Usage on LAN

On any device connected to your network, initialize and push your art:

    # Log in to your desktop server's IP
    gitimg login <username> <password> 192.168.131.96:8080

    # Sync assets (no manual init required)
    gitimg push "First LAN sync"

## API Access

GitImg provides a JSON REST API alongside its embedded dashboard.

* Auth: POST /auth/login
* Metadata: GET /repo/<owner>/<repo>
* Assets: GET /repo/asset/<owner>/<repo>

---
*Built for high-performance artistic workflows.*