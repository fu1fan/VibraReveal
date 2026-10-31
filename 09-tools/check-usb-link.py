#!/usr/bin/env python3
"""Require the image destination route to use Apple's native USB NCM driver.

An ICMP reply is insufficient: a Wi-Fi gateway can route the same private subnet.
This check is read-only and does not change network services, routes or DNS.
"""
import ipaddress
import json
import plistlib
import re
import subprocess
import sys


def run(*args):
    return subprocess.check_output(args, stderr=subprocess.STDOUT)


def names(node):
    if isinstance(node, list):
        for child in node:
            yield from names(child)
    elif isinstance(node, dict):
        name = node.get('BSD Name')
        if isinstance(name, str):
            yield name
        yield from names(node.get('IORegistryEntryChildren', []))


try:
    destination = str(ipaddress.IPv4Address(sys.argv[1] if len(sys.argv) > 1 else '192.168.77.1'))
    # -l includes child IOEthernetInterface properties (including BSD Name).
    registry = run('ioreg', '-r', '-c', 'AppleUSBNCMData', '-a', '-l')
    ncm = plistlib.loads(registry) if registry.strip() else []
    interfaces = set(names(ncm))
    if not interfaces:
        raise RuntimeError('No registered USB NCM network interface. Unlock the Mac screen and reconnect/restart the USB gadget; check configd InterfaceNamer logs.')
    route = run('route', '-n', 'get', destination).decode()
    found = re.search(r'^\s*interface:\s*(\S+)', route, re.M)
    if found is None or found.group(1) not in interfaces:
        raise RuntimeError(f'{destination} does not route over USB NCM (route interface: {found.group(1) if found else "none"}). Do not count this as a USB benchmark.')
    interface = found.group(1)
    local = run('ipconfig', 'getifaddr', interface).decode().strip()
    print(json.dumps({'destination': destination, 'interface': interface,
                      'localAddress': local, 'driver': 'AppleUSBNCMData'}, indent=2))
except (ValueError, RuntimeError, subprocess.CalledProcessError) as error:
    print(f'USB link check failed: {error}', file=sys.stderr)
    sys.exit(1)
