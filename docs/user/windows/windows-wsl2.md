# WSL2

Install a Linux distribution in WSL2, build mogbus there and run it.

## Set up

In PowerShell as administrator, install a distribution and open it:

```powershell
wsl --install -d Ubuntu
```

In the Linux shell, install the build dependencies and build:

```bash
sudo apt update
sudo apt install make clang libssl-dev pkg-config python3 git
git clone --recurse-submodules https://github.com/BolvicBolvicovic/mogbus.git
cd mogbus
make
./mogbus profiles/minimal.scm
```

Without OpenSSL, `make WITH_TLS=0` builds everything except `(tls ...)`. Instead of building, you can
unpack a release tarball (`mogbus-<version>-linux-x86_64.tar.gz` from the GitHub releases) inside
WSL; it needs the OpenSSL runtime library (`libssl3` on Ubuntu 22.04 and later).

Work in the Linux file system (`~/mogbus`), not under `/mnt/c`: builds are much faster there.

## Python in WSL

The Python package works inside WSL as on any Linux machine, see
[Using mogbus from Python](../python/python-start.md):

```bash
make python
pip install ./python
```

## Next

- [Reaching mogbus from Windows](windows-networking.md): the default `bind` only accepts local
  clients, and a Windows program is not the same machine as WSL2 in every network mode.
- [Serial devices](windows-serial.md)

## See also

- [Running mogbus on Windows](windows-route.md)
- [Build from source](../building.md)
