#!/usr/bin/env python3
"""Download and decrypt a public MEGA file link (https://mega.nz/file/<handle>#<key>).

Usage: mega-get.py <url> <output-dir>
Writes <output-dir>/<name from MEGA attributes>, verifies MEGA's CBC-MAC, prints the path.
Refuses to overwrite an existing file. Needs python3-cryptography.
"""
import base64, json, os, struct, sys, urllib.request
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

API = 'https://g.api.mega.co.nz/cs'


def b64(s):
    s = s.replace('-', '+').replace('_', '/').replace(',', '')
    return base64.b64decode(s + '=' * (-len(s) % 4))


def main(url, outdir):
    if not url.startswith('https://mega.nz/file/') or '#' not in url:
        sys.exit('URL must be https://mega.nz/file/<handle>#<key>')
    tail = url.split('/file/', 1)[1]
    handle, key_s = tail.split('#', 1)
    k = struct.unpack('>8I', b64(key_s))
    aes_key = struct.pack('>4I', k[0] ^ k[4], k[1] ^ k[5], k[2] ^ k[6], k[3] ^ k[7])
    iv = struct.pack('>2I', k[4], k[5]) + b'\0' * 8
    meta_mac = struct.pack('>2I', k[6], k[7])

    req = urllib.request.Request(API, data=json.dumps([{'a': 'g', 'g': 1, 'p': handle}]).encode(),
                                 headers={'Content-Type': 'application/json'})
    info = json.load(urllib.request.urlopen(req, timeout=60))[0]
    if not isinstance(info, dict):
        sys.exit(f'MEGA API error: {info}')
    at = Cipher(algorithms.AES(aes_key), modes.CBC(b'\0' * 16)).decryptor().update(b64(info['at']))
    if not at.startswith(b'MEGA{'):
        sys.exit('bad key: attributes did not decrypt')
    name = json.loads(at[4:].rstrip(b'\0').decode())['n']
    if '/' in name or name in ('', '.', '..'):
        sys.exit(f'refusing unsafe file name {name!r}')
    out = os.path.join(outdir, name)
    if os.path.exists(out):
        sys.exit(f'refusing to overwrite {out}')

    enc = urllib.request.urlopen(info['g'], timeout=300).read()
    if len(enc) != info['s']:
        sys.exit(f'size mismatch {len(enc)} != {info["s"]}')
    data = Cipher(algorithms.AES(aes_key), modes.CTR(iv)).decryptor().update(enc)

    # MEGA condensed MAC: per-chunk CBC-MAC (IV = nonce||nonce), chained through AES-ECB.
    ecb = Cipher(algorithms.AES(aes_key), modes.ECB()).encryptor()
    nonce = iv[:8]
    bounds, pos, size = [], 0, 0x20000
    while pos < len(data):
        bounds.append((pos, min(pos + size, len(data))))
        pos += size
        if size < 0x100000:
            size += 0x20000
    file_mac = b'\0' * 16
    for a, b in bounds:
        chunk = data[a:b]
        chunk += b'\0' * (-len(chunk) % 16)
        mac = Cipher(algorithms.AES(aes_key), modes.CBC(nonce + nonce)).encryptor().update(chunk)[-16:]
        file_mac = ecb.update(bytes(x ^ y for x, y in zip(file_mac, mac)))
    m = struct.unpack('>4I', file_mac)
    if struct.pack('>2I', m[0] ^ m[1], m[2] ^ m[3]) != meta_mac:
        sys.exit('MEGA MAC mismatch: download corrupt')

    with open(out, 'xb') as f:
        f.write(data)
    print(out)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
