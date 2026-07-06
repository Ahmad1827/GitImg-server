# GitImg

A custom, high-performance version control system tailored for artists and binary asset management.

GitImg leverages **FastCDC (Content-Defined Chunking)** and **FNV-1a hashing** to provide efficient deduplication and storage for large binary files.

---

## Architecture

GitImg is built using **pure C++17** with **zero external dependencies**, utilizing POSIX/Linux APIs for low-level system interaction.

### Client (`gitimg`)

Performs client-side chunking and pushes only unique data to the server.

### Server (`gitimgd`)

A raw socket daemon responsible for:

* Append-only packfile storage
* Metadata management
* User authentication
* Repository handling
* Asset reconstruction

### Web Interface

An embedded HTTP dashboard dynamically reconstructs assets directly from packfile chunks and renders them in a dark-mode, GitHub-inspired interface.

Example preview:

```text
http://localhost:8080/ahmad/WisdomArt
```

---

## Core Features

### Network Deduplication

Only new or modified binary segments are transferred to the server.

Benefits:

* Reduced bandwidth usage
* Faster uploads
* Efficient storage utilization

---

### Identity & Security

Integrated authentication system including:

* User registration
* Login system
* Session-based token authentication
* Repository permissions

---

### Repository Management

Supports ownership and access control:

* Public repositories
* Private repositories
* Repository owners
* Collaborator permissions

---

### Artist-Focused Interface

GitHub-inspired workflow designed specifically for artists:

* Gallery-style asset browsing
* Commit history feed
* Image previews
* Version tracking

---

## Quick Start

### 1. Compile

```bash
make clean && make
```

### 2. Run Server

```bash
./gitimgd
```

### 3. Authentication & Usage

Register/Login:

```bash
gitimg login <username> <password>
```

Clone a remote repository:

```bash
gitimg clone <owner>/<repo_name>
```

Push changes:

```bash
gitimg push "Commit message"
```

---

## API Access

GitImg exposes a JSON REST API alongside the embedded web dashboard.

### Authentication

```http
POST /auth/login
```

---

### Repository Metadata

```http
GET /repo/<owner>/<repo>
```

---

### Asset Access

```http
GET /repo/<owner>/<repo>/asset/<asset_id>
```

---

## Design Goals

GitImg aims to provide:

* High-performance binary versioning
* Efficient deduplication
* Git-like workflows for artists
* Lightweight infrastructure
* GitHub-style collaboration for creative assets

---

Built for high-performance artistic workflows.
