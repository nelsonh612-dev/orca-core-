#!/usr/bin/env python3
"""Reads the Wii test images written by orca_wii_reference_fixture the way
Dolphin does, independently of Orca's code, and compares the results with
Orca's (orca.jsonl).

The steps follow Dolphin's source (read as information):
  DiscIO/VolumeWii.cpp   VolumeWii::VolumeWii (0x60/0x61 flags, four partition
                         groups at 0x40000, offsets >> 2), Read, DecryptBlockHashes
                         (AES-CBC, IV 0, 0x400 bytes), DecryptBlockData (IV = the
                         encrypted bytes 0x3D0-0x3DF, 0x7C00 bytes from 0x400),
                         CheckBlockIntegrity (H0 31 x SHA-1 of 0x400; SHA-1 of the
                         H0 table == H1[block % 8]; SHA-1 of H1 == H2[block / 8 % 8];
                         SHA-1 of H2 == H3[block / 64]), CheckH3TableIntegrity (one
                         TMD content whose SHA-1 is that of the 0x18000-byte H3 table)
  Core/IOS/ES/Formats.cpp TicketReader::GetTitleKey (IV = title ID at 0x1DC + 8 zero
                         bytes, encrypted key at 0x1BF, common key index at 0x1F1;
                         index >= 2 falls back to 0)
AES-128-CBC comes from the openssl command, SHA-1 from hashlib.

Usage: dolphin_wii_reference.py DIR      (exit 0 = identical, 1 = differences,
                                          77 = openssl not available)
"""
import hashlib
import json
import shutil
import subprocess
import sys


def aes_cbc_decrypt(key, iv, data):
    r = subprocess.run(["openssl", "enc", "-d", "-aes-128-cbc", "-nopad", "-K", key.hex(), "-iv", iv.hex()],
                       input=data, capture_output=True, check=True)
    return r.stdout


def be32(b, o):
    return int.from_bytes(b[o:o + 4], "big")


def sha1(b):
    return hashlib.sha1(b).digest()


def read_keys(path):
    keys = {}
    for line in open(path):
        name, _, value = line.partition("=")
        keys[name.strip()] = bytes.fromhex(value.strip())
    return [keys.get("common_key"), keys.get("korean_common_key")]


def dolphin_read(image, img, common):
    out = []
    has_hashes = img[0x60] == 0
    has_encryption = img[0x61] == 0
    partitions = []
    for group in range(4):
        count = be32(img, 0x40000 + group * 8)
        table = be32(img, 0x40000 + group * 8 + 4) << 2
        for i in range(count):
            partitions.append(be32(img, table + i * 8) << 2)
    for index, p in enumerate(partitions):
        ticket = img[p:p + 0x2A4]
        title_id = ticket[0x1DC:0x1E4]
        ck = ticket[0x1F1]
        if ck >= 2:  # TicketReader::GetTitleKey: "Bad common key index ... using common key 0"
            ck = 0
        title_key = aes_cbc_decrypt(common[ck], title_id + bytes(8), ticket[0x1BF:0x1CF])
        tmd_size = be32(img, p + 0x2A4)
        tmd_off = be32(img, p + 0x2A8) << 2
        h3_off = be32(img, p + 0x2B4) << 2
        data_off = be32(img, p + 0x2B8) << 2
        data_size = be32(img, p + 0x2BC) << 2
        tmd = img[p + tmd_off:p + tmd_off + tmd_size]
        h3 = img[p + h3_off:p + h3_off + 0x18000]
        contents = int.from_bytes(tmd[0x1DE:0x1E0], "big")
        h3_ok = contents == 1 and sha1(h3) == tmd[0x1E4 + 0x10:0x1E4 + 0x24]
        rec = {"image": image, "partition": index, "title_id": title_id.hex(), "title_key_sha1": sha1(title_key).hex(), "h3_ok": h3_ok}
        blocks, data = [], b""
        for b in range(data_size // 0x8000):
            raw = img[p + data_off + b * 0x8000:p + data_off + (b + 1) * 0x8000]
            if has_encryption:
                hashes = aes_cbc_decrypt(title_key, bytes(16), raw[:0x400])           # DecryptBlockHashes
                block = aes_cbc_decrypt(title_key, raw[0x3D0:0x3E0], raw[0x400:])      # DecryptBlockData
            else:
                hashes, block = raw[:0x400], raw[0x400:]
            ok = has_hashes
            ok = ok and all(sha1(block[i * 0x400:(i + 1) * 0x400]) == hashes[i * 20:(i + 1) * 20] for i in range(31))
            ok = ok and sha1(hashes[:0x26C]) == hashes[0x280 + 20 * (b % 8):0x280 + 20 * (b % 8) + 20]
            ok = ok and sha1(hashes[0x280:0x320]) == hashes[0x340 + 20 * (b // 8 % 8):0x340 + 20 * (b // 8 % 8) + 20]
            ok = ok and sha1(hashes[0x340:0x3E0]) == h3[20 * (b // 64):20 * (b // 64) + 20]
            blocks.append(ok)
            data += block
        rec["blocks"] = blocks
        rec["data_sha1"] = sha1(data).hex()
        out.append(rec)
    return out


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    if shutil.which("openssl") is None:
        print("[dolphin_wii_reference] SKIPPED: the openssl command is not available")
        return 77
    d = sys.argv[1]
    common = read_keys(d + "/keys.txt")
    orca = [json.loads(line) for line in open(d + "/orca.jsonl") if line.strip()]
    images = sorted({r["image"] for r in orca})
    failures = 0
    compared = 0
    for image in images:
        img = open(d + "/" + image, "rb").read()
        ref = dolphin_read(image, img, common)
        mine = [r for r in orca if r["image"] == image]
        if len(ref) != len(mine):
            print(f"{image}: Dolphin procedure finds {len(ref)} partitions, Orca {len(mine)}")
            failures += 1
            continue
        for r, o in zip(ref, mine):
            where = f"{image} partition {r['partition']}"
            if "open_error" in o:
                # Orca refuses to open a partition whose H3 table or block 0 is bad;
                # Dolphin opens it and fails the same check when verifying.
                expected = "h3" if not r["h3_ok"] else ("block0" if not r["blocks"][0] else None)
                same = o["open_error"] == expected
                print(f"{where}: Orca refuses to open it ({o['open_error']}); Dolphin procedure: H3 {'ok' if r['h3_ok'] else 'BAD'}, "
                      f"block 0 {'ok' if r['blocks'][0] else 'BAD'} -> {'same verdict' if same else 'DIFFERENT'}")
                failures += 0 if same else 1
                compared += 1
                continue
            for field in ("title_id", "title_key_sha1", "h3_ok", "blocks"):
                if r[field] != o[field]:
                    print(f"{where}: {field} differs: Dolphin procedure {r[field]}, Orca {o[field]}")
                    failures += 1
            if all(r["blocks"]):
                if r["data_sha1"] != o.get("data_sha1"):
                    print(f"{where}: decrypted data differs")
                    failures += 1
            bad = [i for i, ok in enumerate(r["blocks"]) if not ok]
            print(f"{where}: title {r['title_id']}, {len(r['blocks'])} blocks, bad blocks {bad or 'none'}"
                  f"{', data identical' if all(r['blocks']) and r['data_sha1'] == o.get('data_sha1') else ''}")
            compared += 1
    print(f"[dolphin_wii_reference] {compared} partitions compared, {failures} differences")
    return 0 if failures == 0 and compared > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
