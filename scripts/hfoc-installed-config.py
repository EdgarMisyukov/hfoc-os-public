#!/usr/bin/env python3
"""Generate the installed system's boot configuration (ITEM-399, ITEM-446).

The installer medium seals the *installed* system's boot configuration and the
boot stage stages it verbatim into the installed ESP, so the appliance that runs
from the disk boots the configuration the medium carries.  What that
configuration never carried was the operator's management scope: the media
declaration records `HFOC-INSTALL-MANAGEMENT=`, but the installed entry had no
command line at all, so an installed appliance fell back to the kernel's
deny-by-default `LOCAL_ONLY` whether or not the operator had opted the
management interface in.

This tool turns the sealed template (`boot/hfoc-installed.conf`) into the
configuration the installed appliance actually boots:

  * every template line is copied verbatim, so the entry still names the kernel
    the boot stage stages (`PRODUCTION_INSTALLER_BOOT_CONFIG_REFERENCE`,
    `path: boot():/boot/hfoc-kernel.elf`) and still carries the two
    `module_path:` lines that make the installed disk's own boot chain
    verifiable (`[BOOT-CHAIN] verdict=ATTESTED`);
  * exactly one `cmdline:` line is inserted into the template's entry, naming
    the scope the operator declared on the medium:
    `hfoc.management=management-interface` or `hfoc.management=local-only`.
    The scope is written either way, so the installed system states it instead
    of relying on a default;
  * ITEM-449: the same line carries the forward the operator declared,
    `hfoc.forward=on hfoc.forward.target=... hfoc.forward.target-port=...
    hfoc.forward.listen-port=... hfoc.forward.clients=...` or
    `hfoc.forward=off`.  The target is a literal IPv4 address, the ports are
    1..65535 (the management serving port 443 is refused as a listen port), and
    the client list is one to four exact addresses; anything else refuses the
    build here, and the kernel refuses the install before the first write as
    well.  The access rule is that list: an absent list is not "everyone";
  * ITEM-446: the same single line carries the network the operator declared,
    `hfoc.network=dhcp` or
    `hfoc.network=static hfoc.address=... hfoc.gateway=... hfoc.dns=...`, so
    the installed appliance comes up on the address the operator chose instead
    of discovering one.  A static declaration is only accepted when it is
    complete and when the address parses, the prefix is 1..32 and the gateway
    lies inside the declared subnet; anything else refuses the build here, and
    the kernel refuses the install before the first write as well.

The output is a pure function of the template and the scope, so two builds of
the same inputs produce the same bytes - the ITEM-371 sealed media manifest and
the ITEM-372 sealed boot chain then seal exactly what the installer stages
(ITEM-397 discipline: the bytes staged are the bytes sealed).

Usage:
  hfoc-installed-config.py --template FILE --management local-only|management-interface \
      [--network dhcp|static --address A.B.C.D/PREFIX --gateway A.B.C.D --dns A.B.C.D] \
      [--forward on|off --forward-target A.B.C.D --forward-target-port N \
       --forward-listen-port N --forward-clients A.B.C.D[,A.B.C.D]] \
      --output FILE
"""

import argparse
import ipaddress
import pathlib
import sys

MANAGEMENT_LOCAL = "local-only"
MANAGEMENT_INTERFACE = "management-interface"
CMDLINE_PREFIX = "cmdline:"
# The line the boot stage requires the staged configuration to carry: the
# configuration must locate the kernel it is staged with
# (`PRODUCTION_INSTALLER_BOOT_CONFIG_REFERENCE`).
KERNEL_REFERENCE = "path: boot():/boot/hfoc-kernel.elf"
SCOPES = {
    MANAGEMENT_LOCAL: "hfoc.management=local-only",
    MANAGEMENT_INTERFACE: "hfoc.management=management-interface",
}
NETWORK_DHCP = "dhcp"
NETWORK_STATIC = "static"
# ITEM-449: the management serving listener owns this port; a forward rule that
# claimed it could not be honoured, so the declaration is refused here.
SERVING_LISTEN_PORT = 443
FORWARD_ON = "on"
FORWARD_OFF = "off"
FORWARD_CLIENT_MAX = 4


def forward_address(text, label):
    """Return one literal IPv4 host address, or refuse the declaration.

    The kernel refuses the unspecified address, the limited broadcast, the
    loopback range and the multicast range as a target or a client - none of
    them is a host a forward rule can mean - so the build refuses them too.
    """

    try:
        address = ipaddress.ip_address(text)
    except ValueError as error:
        raise SystemExit(f"invalid forward {label}: {error}")
    if address.version != 4:
        raise SystemExit(f"the forward {label} must be IPv4")
    if (address.is_unspecified or address.is_loopback or
            address.is_multicast or
            address == ipaddress.ip_address("255.255.255.255")):
        raise SystemExit(f"the forward {label} {address} is not a host")
    return str(address)


def forward_port(text, label, reserved=False):
    """Return one bounded TCP port, or refuse the declaration."""

    try:
        port = int(text)
    except (TypeError, ValueError):
        raise SystemExit(f"the forward {label} must be a decimal port")
    if not (1 <= port <= 65535):
        raise SystemExit(f"the forward {label} must be between 1 and 65535")
    if reserved and port == SERVING_LISTEN_PORT:
        raise SystemExit(
            f"the forward {label} cannot claim the serving port "
            f"{SERVING_LISTEN_PORT}")
    return port


def forward_fragment(forward, target, target_port, listen_port, clients):
    """Return the one forward declaration the installed entry carries.

    The values are the operator's declaration on the medium; the kernel
    validates the same declaration again, before the first write and again at
    boot, so a value that slips through here cannot reach the appliance.
    """

    values = (target, target_port, listen_port, clients)
    if not forward:
        if any(values):
            raise SystemExit(
                "a forward value needs --forward on or --forward off")
        return ""
    if forward == FORWARD_OFF:
        if any(values):
            raise SystemExit("--forward off cannot carry forward values")
        return "hfoc.forward=off"
    if forward != FORWARD_ON:
        raise SystemExit(f"unknown forward mode: {forward!r}")
    if not all(values):
        raise SystemExit(
            "an enabled forward requires --forward-target, "
            "--forward-target-port, --forward-listen-port and "
            "--forward-clients")
    target_address = forward_address(target, "target")
    target_port_value = forward_port(target_port, "target port")
    listen_port_value = forward_port(listen_port, "listen port", reserved=True)
    client_values = clients.split(",")
    if not (1 <= len(client_values) <= FORWARD_CLIENT_MAX):
        raise SystemExit(
            f"the forward client list must hold 1 to {FORWARD_CLIENT_MAX} "
            "addresses")
    checked = [forward_address(client, "client") for client in client_values]
    if len(set(checked)) != len(checked):
        raise SystemExit("the forward client list repeats an address")
    return (f"hfoc.forward=on hfoc.forward.target={target_address} "
            f"hfoc.forward.target-port={target_port_value} "
            f"hfoc.forward.listen-port={listen_port_value} "
            f"hfoc.forward.clients={','.join(checked)}")


def network_fragment(network, address, gateway, dns):
    """Return the one network declaration the installed entry carries.

    The values are the operator's declaration on the medium, and this function
    is the build-time sanity check: the kernel validates the same declaration
    again, before the first write and again at boot, so a value that slips
    through here cannot reach the installer.
    """

    if not network:
        if address or gateway or dns:
            raise SystemExit(
                "refusing an address, gateway or DNS without a network mode")
        return ""
    if network == NETWORK_DHCP:
        if address or gateway or dns:
            raise SystemExit(
                "refusing a static value with --network dhcp")
        return "hfoc.network=dhcp"
    if network != NETWORK_STATIC:
        raise SystemExit(f"unknown network mode: {network!r}")
    if not (address and gateway and dns):
        raise SystemExit(
            "a static declaration requires --address, --gateway and --dns")
    try:
        interface = ipaddress.ip_interface(address)
        gateway_address = ipaddress.ip_address(gateway)
        dns_address = ipaddress.ip_address(dns)
    except ValueError as error:
        raise SystemExit(f"invalid static declaration: {error}")
    if interface.version != 4:
        raise SystemExit("only an IPv4 address is accepted")
    if not (1 <= interface.network.prefixlen <= 32):
        raise SystemExit("the prefix must be between 1 and 32")
    if gateway_address not in interface.network:
        raise SystemExit(
            f"the gateway {gateway_address} is outside {interface.network}")
    return (f"hfoc.network=static hfoc.address={address} "
            f"hfoc.gateway={gateway} hfoc.dns={dns}")


def generate(template, management, network="", address="", gateway="", dns="",
             forward="", forward_target="", forward_target_port="",
             forward_listen_port="", forward_clients=""):
    """Return the installed configuration bytes for one declared scope."""

    if management not in SCOPES:
        raise SystemExit(
            f"unknown management scope: {management!r} "
            f"(expected {'|'.join(sorted(SCOPES))})")
    if not template.strip():
        raise SystemExit("refusing an empty installed-configuration template")
    lines = template.split("\n")
    # A trailing newline in the template becomes an empty final element; the
    # output keeps exactly the line structure the template had.
    if lines and lines[-1] == "":
        lines = lines[:-1]
        trailing_newline = True
    else:
        trailing_newline = False

    entry_lines = [
        index for index, line in enumerate(lines)
        if line.strip() == KERNEL_REFERENCE
    ]
    if len(entry_lines) != 1:
        raise SystemExit(
            f"the template must name the staged kernel exactly once "
            f"({KERNEL_REFERENCE!r}): found {len(entry_lines)}")
    if any(line.strip().startswith(CMDLINE_PREFIX) for line in lines):
        raise SystemExit(
            "the template already declares a cmdline; the generated "
            "configuration would carry two declarations")
    for line in lines:
        if "hfoc.management=" in line:
            raise SystemExit(
                "the template already carries a management scope: " + line)

    reference_index = entry_lines[0]
    # The indentation of the entry's own keys, so the inserted line is written
    # in the same style as the line it follows.
    indent = lines[reference_index][
        :len(lines[reference_index]) - len(lines[reference_index].lstrip())]
    fragment = network_fragment(network, address, gateway, dns)
    forward_declaration = forward_fragment(
        forward, forward_target, forward_target_port, forward_listen_port,
        forward_clients)
    declaration = SCOPES[management]
    if fragment:
        declaration = f"{declaration} {fragment}"
    if forward_declaration:
        declaration = f"{declaration} {forward_declaration}"
    inserted = f"{indent}{CMDLINE_PREFIX} {declaration}"
    output = lines[:reference_index + 1] + [inserted] + lines[reference_index + 1:]
    text = "\n".join(output)
    if trailing_newline:
        text += "\n"
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--template", required=True,
                        help="the sealed boot/limine configuration template")
    parser.add_argument("--management", required=True,
                        choices=sorted(SCOPES),
                        help="the scope the operator declared on the medium")
    parser.add_argument("--network", default="",
                        choices=["", NETWORK_DHCP, NETWORK_STATIC],
                        help="the network the operator declared on the medium")
    parser.add_argument("--address", default="",
                        help="a.b.c.d/prefix for --network static")
    parser.add_argument("--gateway", default="",
                        help="the gateway for --network static")
    parser.add_argument("--dns", default="",
                        help="the DNS server for --network static")
    parser.add_argument("--forward", default="",
                        choices=["", FORWARD_ON, FORWARD_OFF],
                        help="the forward the operator declared on the medium")
    parser.add_argument("--forward-target", default="",
                        help="the literal IPv4 target of the forward")
    parser.add_argument("--forward-target-port", default="",
                        help="the TCP port on the target")
    parser.add_argument("--forward-listen-port", default="",
                        help="the local TCP port the appliance listens on")
    parser.add_argument("--forward-clients", default="",
                        help="the allowed client addresses, comma separated")
    parser.add_argument("--output", required=True)
    arguments = parser.parse_args()

    template = pathlib.Path(arguments.template).read_text()
    text = generate(template, arguments.management, arguments.network,
                    arguments.address, arguments.gateway, arguments.dns,
                    arguments.forward, arguments.forward_target,
                    arguments.forward_target_port,
                    arguments.forward_listen_port,
                    arguments.forward_clients)
    if KERNEL_REFERENCE not in text:
        raise SystemExit(
            "the generated configuration lost the staged-kernel reference")
    pathlib.Path(arguments.output).write_text(text)
    print("installed configuration: "
          f"management={arguments.management} "
          f"network={arguments.network or 'default'} "
          f"forward={arguments.forward or 'default'} "
          f"bytes={len(text.encode())}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
