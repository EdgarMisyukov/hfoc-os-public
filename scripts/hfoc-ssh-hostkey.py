#!/usr/bin/env python3
"""ITEM-613: turn the diagnostic session's public SSH host-key record into the
files an operator needs, without a photograph or a manual base64 copy.

The appliance publishes one `[PRODUCTION-SSH-HOSTKEY]` record through the normal
diagnostic snapshot.  This module parses that record, validates the OpenSSH
public blob and its declared SHA256 fingerprint, and writes:

    HFOC-SSH-HOST-<session>.pub
    HFOC-SSH-HOST-<session>.fingerprint
    HFOC-SSH-HOST-<session>.known_hosts.candidate

Only the public blob and fingerprint are written.  Nothing here reads or creates
a private key, a session token, a seed or a password.

The module is also the reader's test seam (`--session FILE --output DIR`), so the
host proof drives exactly the code the Debian extractor uses.
"""
import argparse
import base64
import binascii
import hashlib
import pathlib
import re
import sys

ALGORITHM = "ecdsa-sha2-nistp256"
CURVE = "nistp256"
RECORD = re.compile(r"^\[PRODUCTION-SSH-HOSTKEY\] (.*)$")
FIELD = re.compile(r"([a-z_]+)=([^ ]+)")
FP = re.compile(r"^SHA256:[A-Za-z0-9+/]{43}$")


class HostKeyError(ValueError):
    pass


def parse_record(lines):
    """The session's host-key record, refusing a contradicting duplicate."""
    records = []
    for line in lines:
        match = RECORD.match(line)
        if match is not None:
            fields = dict(FIELD.findall(match.group(1)))
            if not fields:
                raise HostKeyError("the host-key record carries no fields")
            records.append(fields)
    if not records:
        return None
    ready = [fields for fields in records if fields.get("state") == "READY"]
    if len(ready) > 1:
        first = (ready[0].get("blob"), ready[0].get("fingerprint"))
        for fields in ready[1:]:
            if (fields.get("blob"), fields.get("fingerprint")) != first:
                raise HostKeyError(
                    "the session carries contradicting host-key records")
    if ready:
        return ready[-1]
    return records[-1]


def _read_string(blob, offset, what):
    if offset + 4 > len(blob):
        raise HostKeyError(f"the public blob ends before the {what} length")
    length = int.from_bytes(blob[offset:offset + 4], "big")
    offset += 4
    if length > len(blob) - offset:
        raise HostKeyError(f"the public blob ends inside the {what}")
    return blob[offset:offset + length], offset + length


def validate_blob(blob_text, declared_algorithm, declared_fingerprint):
    if not isinstance(blob_text, str) or not blob_text:
        raise HostKeyError("the public blob is empty")
    try:
        blob = base64.b64decode(blob_text, validate=True)
    except (binascii.Error, ValueError) as error:
        raise HostKeyError(f"the public blob is not strict base64: {error}")
    if base64.b64encode(blob).decode("ascii") != blob_text:
        raise HostKeyError("the public blob is not canonical padded base64")
    if declared_algorithm != ALGORITHM:
        raise HostKeyError(f"unexpected algorithm {declared_algorithm}")
    offset = 0
    algorithm, offset = _read_string(blob, offset, "algorithm")
    if algorithm != ALGORITHM.encode("ascii"):
        raise HostKeyError("the public blob's algorithm does not match")
    curve, offset = _read_string(blob, offset, "curve")
    if curve != CURVE.encode("ascii"):
        raise HostKeyError("the public blob's curve does not match")
    point, offset = _read_string(blob, offset, "point")
    if len(point) != 65 or point[0] != 0x04:
        raise HostKeyError("the public blob's point is not uncompressed P-256")
    if offset != len(blob):
        raise HostKeyError("the public blob carries trailing bytes")
    fingerprint = "SHA256:" + base64.b64encode(
        hashlib.sha256(blob).digest()).decode("ascii").rstrip("=")
    if declared_fingerprint != fingerprint:
        raise HostKeyError(
            f"the declared fingerprint {declared_fingerprint} does not match "
            f"the independently computed {fingerprint}")
    if not FP.match(fingerprint):
        raise HostKeyError("the computed fingerprint has the wrong shape")
    return blob, fingerprint


def host_pattern(address, port):
    if address in (None, "", "none"):
        return None
    if not re.match(r"^[0-9]{1,3}(\.[0-9]{1,3}){3}$", address):
        raise HostKeyError(f"the record's address is not IPv4: {address}")
    octets = [int(part) for part in address.split(".")]
    if any(part < 0 or part > 255 for part in octets):
        raise HostKeyError(f"the record's address is not IPv4: {address}")
    if port < 1 or port > 65535:
        raise HostKeyError(f"the record's port is invalid: {port}")
    return address if port == 22 else f"[{address}]:{port}"


def write_candidate(path, text):
    """Write without clobbering a different existing pin."""
    path = pathlib.Path(path)
    if path.exists():
        existing = path.read_text(encoding="ascii", errors="replace")
        if existing == text:
            return "kept", path
        candidate = path.with_name(path.name + ".new")
        candidate.write_text(text, encoding="ascii")
        candidate.chmod(0o600)
        return "conflict", candidate
    path.write_text(text, encoding="ascii")
    path.chmod(0o600)
    return "written", path


def export_session(lines, output_dir, session_id):
    """Validate one extracted session and write its public files."""
    fields = parse_record(lines)
    if fields is None:
        return {"state": "ABSENT", "reason": "NO_HOSTKEY_RECORD"}
    if fields.get("state") != "READY":
        return {"state": "UNAVAILABLE",
                "reason": fields.get("reason", "UNKNOWN")}
    required = ("algorithm", "blob", "fingerprint")
    for name in required:
        if name not in fields:
            raise HostKeyError(f"the host-key record lacks {name}=")
    blob, fingerprint = validate_blob(
        fields["blob"], fields["algorithm"], fields["fingerprint"])
    try:
        port = int(fields.get("port", "22"))
    except ValueError:
        raise HostKeyError("the record's port is not a number")
    pattern = host_pattern(fields.get("address", "none"), port)
    output_dir = pathlib.Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    stem = output_dir / f"HFOC-SSH-HOST-{session_id:08X}"
    public = f"{ALGORITHM} {base64.b64encode(blob).decode('ascii')}"
    pub_path = stem.with_suffix(".pub")
    fp_path = stem.with_suffix(".fingerprint")
    pub_state, pub_target = write_candidate(pub_path, public + "\n")
    fp_state, fp_target = write_candidate(fp_path, fingerprint + "\n")
    result = {
        "state": "OK",
        "algorithm": ALGORITHM,
        "fingerprint": fingerprint,
        "pub": str(pub_target),
        "pub_state": pub_state,
        "fingerprint_file": str(fp_target),
        "fingerprint_state": fp_state,
        "session": f"{session_id:08X}",
        "build": fields.get("build", "unknown"),
        "address": fields.get("address", "none"),
        "port": port,
    }
    if pattern is None:
        result["known_hosts"] = None
        result["known_hosts_reason"] = "NO_ADDRESS"
        return result
    known = f"{pattern} {public}\n"
    kh_path = stem.with_suffix(".known_hosts.candidate")
    kh_state, kh_target = write_candidate(kh_path, known)
    result["known_hosts"] = str(kh_target)
    result["known_hosts_state"] = kh_state
    result["host"] = pattern
    return result


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", required=True,
                        help="extracted HFOC-DIAG-<session>.txt")
    parser.add_argument("--output", default=".",
                        help="directory for the public files")
    parser.add_argument("--session-id", default=None,
                        help="session hex (derived from the file name otherwise)")
    args = parser.parse_args(argv)
    session = pathlib.Path(args.session)
    if not session.is_file():
        print(f"ssh-hostkey: no such session file: {session}", file=sys.stderr)
        return 1
    session_id = args.session_id
    if session_id is None:
        match = re.search(r"HFOC-DIAG-([0-9A-Fa-f]{8})", session.name)
        session_id = match.group(1) if match else "00000000"
    try:
        result = export_session(
            session.read_text(encoding="ascii", errors="replace").splitlines(),
            args.output, int(session_id, 16))
    except HostKeyError as error:
        print(f"ssh-hostkey: REFUSED {error}", file=sys.stderr)
        return 2
    if result["state"] == "ABSENT":
        print("ssh-hostkey: hostkey=ABSENT reason=NO_HOSTKEY_RECORD")
        return 0
    if result["state"] == "UNAVAILABLE":
        print(f"ssh-hostkey: hostkey=UNAVAILABLE reason={result['reason']}")
        return 0
    print(f"ssh-hostkey: fingerprint={result['fingerprint']}")
    print(f"ssh-hostkey: pub={result['pub']} ({result['pub_state']})")
    print(f"ssh-hostkey: session={result['session']} build={result['build']}")
    if result.get("known_hosts"):
        print(f"ssh-hostkey: known_hosts={result['known_hosts']} "
              f"({result['known_hosts_state']}) host={result['host']}")
    else:
        print(f"ssh-hostkey: known_hosts=UNAVAILABLE "
              f"reason={result.get('known_hosts_reason', 'NO_ADDRESS')}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
