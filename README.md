
# File Transfer Application

A secure client-server file transfer system with authentication, resumable downloads, and access control.

---

## Table of Contents

- [Features](#features)  
- [Prerequisites](#prerequisites)  
- [Installation](#installation)  
- [Usage](#usage)  
- [Configuration](#configuration)  
- [Security](#security)  
- [Troubleshooting](#troubleshooting)  
- [Contributing](#contributing)  
- [License](#license)  

---

## Features

- 🔐 Secure password hashing (SHA-256)  
- 🔄 Resumable file transfers  
- 🌐 File visibility options (Public/Private)  
- 🔑 Access code protection for private files  
- 📊 Transfer progress tracking  
- 🖥️ Server administration console  
- 🧾 User transaction history  
- 👥 Concurrent client support  

---

## Prerequisites

- Linux environment  
- GCC compiler  
- SQLite3 development libraries  
- OpenSSL libraries  
- pthread support  

**For Ubuntu/Debian:**

```bash
sudo apt-get install build-essential libsqlite3-dev libssl-dev
```

---

## Installation

Clone the repository:

```bash
git clone https://github.com/yourusername/file-transfer-app.git
cd file-transfer-app
```

Compile the project:

```bash
make        # Build both server and client
make server # Build only the server
make client # Build only the client
make clean  # Remove binaries
```

**Makefile Sample:**

```makefile
CC = gcc
CFLAGS = -Wall -Wextra
LDFLAGS = -pthread -lsqlite3 -lcrypto

all: server client

server: src/server.c
	$(CC) $(CFLAGS) src/server.c -o server $(LDFLAGS)

client: src/client.c
	$(CC) $(CFLAGS) src/client.c -o client $(LDFLAGS) -lcrypto

clean:
	rm -f server client
```

---

## Usage

### Start the Server

```bash
./server
```

### Run the Client

```bash
./client <server_ip>
```

Example:

```bash
./client 127.0.0.1
```

### Basic Client Commands

```
/upload <filename>      # Upload a file
/download <filename>    # Download a file
/list                   # List server files
/locallist              # List local files
/help                   # Show help menu
```

---

## Configuration

### Server Settings

- **Port**: Change `PORT` in `server.c` (default: 8080)  
- **Max Clients**: Modify `MAX_CLIENTS` in `server.c`  
- **File Storage**: Update `files/` directory in `server.c`  

### Client Settings

- **Download Directory**: Modify `DOWNLOAD_FOLDER` in `client.c`  
- **Buffer Size**: Adjust `BUFFER_SIZE` for transfers  

---

## Security

- 🔐 Passwords hashed using SHA-256  
- 🚫 Input validation during authentication  
- 🔏 Access code enforcement for private files  
- 🛡️ SQL injection prevention  
- 🔍 File integrity checks  
- 💼 Basic session management  

**Security Notes:**

- Passwords are never stored or transmitted in plain text  
- Access codes are encrypted in the database  
- Use only in protected/internal networks (not production-ready)  
- For production, add SSL/TLS encryption  

---

## Troubleshooting

### Missing Dependencies?

```bash
sudo apt-get install libsqlite3-dev libssl-dev
```

### Connection Issues?

- Is the server running?  
- Are the firewall rules correct?  
- Is the server IP valid?  

### File Permission Issues?

```bash
chmod +x server client
mkdir -p files client_downloads
```

### Database Errors?

```bash
rm users.db  # ⚠️ WARNING: Deletes all user data
```

---

## Contributing

1. Fork the repository  
2. Create a feature branch:  
   ```bash
   git checkout -b feature/your-feature
   ```  
3. Commit your changes  
4. Push to your branch  
5. Open a Pull Request  

**Development Guidelines:**

- Use C99 standard  
- Maintain consistent code formatting  
- Document any new functions  
- Test thoroughly  
- Keep commits focused and atomic  

---

## License

This project is licensed under the [MIT License](LICENSE).
