#!/usr/bin/env python3
"""Audit pinned upstream ARM64 driver releases. Never installs drivers."""
import hashlib
import io
import json
import pathlib
import re
import struct
import urllib.request
import zipfile

OUT = pathlib.Path("arm64-driver-audit")
OUT.mkdir(exist_ok=True)
MACHINE = {0x014C: "x86", 0x8664: "x64", 0xAA64: "ARM64", 0xA641: "ARM64EC"}
RELEASES = [
    ("vdd", "qiin2333/zako-vdd", "v0.17.5", "zakovdd.zip",
     "e4177a0c03cb0778e0cf963f552f02c295ab855e0600083f9413446b8c238f6f"),
    ("vmouse", "AlkaidLab/zako-vmouse-release", "v1.3.2", "ZakoVirtualMouse.dll",
     "d85afe625e100e1d702737e153ad995a8cce21d5f264170d13781fb392e98ce4"),
    ("vmouse", "AlkaidLab/zako-vmouse-release", "v1.3.2", "ZakoVirtualMouse.inf",
     "0c42096ad1321ac6bc1c8b398ac3f6c3df990b1d7c4cccd9da3c1ec2c54e7fbd"),
    ("vmouse", "AlkaidLab/zako-vmouse-release", "v1.3.2", "ZakoVirtualMouse.cat",
     "9e2118bc24f3d4d4382197d81a02c0924e85c7546385c8d678a4b5c025d2543c"),
    ("vmouse", "AlkaidLab/zako-vmouse-release", "v1.3.2", "ZakoVirtualMouse.cer",
     "5345f6ee35a9907c84e96e5f4575e8950a498513b35685f1811185f8b44868b7"),
    ("gamepad", "nefarius/ViGEmBus", "v1.22.0", "ViGEmBus_1.22.0_x64_x86_arm64.exe",
     "89220a7865076b342892f98865f3499fb7c4cfd673159e89d352c360fd014c6a"),
]

def inspect(name, blob):
    result = {"file": name, "bytes": len(blob)}
    if len(blob) >= 0x44 and blob[:2] == b"MZ":
        offset, = struct.unpack_from("<I", blob, 0x3c)
        if 0x40 <= offset < len(blob) - 24 and blob[offset:offset+4] == b"PE\0\0":
            code, = struct.unpack_from("<H", blob, offset+4)
            result.update(machine=f"0x{code:04X}", architecture=MACHINE.get(code, "unknown"))
        else:
            raise ValueError(f"Malformed PE: {name}")
    if name.lower().endswith(".inf"):
        contents = blob.decode("utf-16" if blob[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8-sig", "replace")
        result["arm64_inf_match"] = "ntarm64" in contents.lower()
        result["requires_stampinf"] = "$ARCH$" in contents or "$UMDFVERSION$" in contents
        result["manufacturer"] = [x.strip() for x in contents.splitlines() if "ntarm64" in x.lower() or "nt$arch$" in x.lower()][:12]
    return result

def download(repo, version, name, expected):
    url = f"https://github.com/{repo}/releases/download/{version}/{name}"
    request = urllib.request.Request(url, headers={"User-Agent":"Sunshine-ARM64-driver-audit"})
    with urllib.request.urlopen(request, timeout=90) as response:
        data = response.read(65_000_001)
    digest = hashlib.sha256(data).hexdigest()
    if len(data) > 65_000_000 or digest.lower() != expected:
        raise RuntimeError(f"Digest/size mismatch for {url}; actual SHA256={digest}")
    return url, data

def main():
    records = []
    for kind, repo, tag, name, sha in RELEASES:
        url, data = download(repo, tag, name, sha)
        (OUT / (kind+"_"+name)).write_bytes(data)
        if name.lower().endswith(".zip"):
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                files = []
                for entry in archive.infolist():
                    if entry.is_dir():
                        continue
                    normal = pathlib.PurePosixPath(entry.filename.replace("\\","/"))
                    if normal.is_absolute() or ".." in normal.parts or entry.file_size > 25_000_000:
                        raise RuntimeError("Unsafe driver archive member: "+entry.filename)
                    files.append(inspect(entry.filename, archive.read(entry)))
        else:
            files = [inspect(name, data)]
        records.append({"family":kind, "repo":repo, "tag":tag, "asset":name,
                        "source":url, "sha256":sha, "verified":True, "files":files})
        print(kind, name, "verified", [(f["file"], f.get("architecture","metadata")) for f in files], flush=True)
    report = {"target":"Windows ARM64","driver_install_attempted":False,
              "test_signing_enabled":False,"driver_signature_acceptance":False,
              "releases":records}
    (OUT / "manifest.json").write_text(json.dumps(report,indent=2,ensure_ascii=False),encoding="utf-8")
    lines = ["# ARM64 Sunshine drivers - read-only audit", "",
             "Hashes are verified; no driver is installed. Architecture alone is not driver compatibility.",
             "", "| Component | Artifact | PE architecture or INF match |", "|---|---|---|"]
    for row in records:
        summary = "; ".join(x["file"]+" "+x.get("architecture", "ARM64 INF" if x.get("arm64_inf_match") else "metadata")
                            for x in row["files"] if x["file"].lower().endswith((".exe",".dll",".sys",".inf")))
        lines.append("| "+row["family"]+" | "+row["asset"]+" | "+summary[:500]+" |")
    (OUT / "README.md").write_text("\n".join(lines)+"\n",encoding="utf-8")

if __name__=="__main__":
    main()
