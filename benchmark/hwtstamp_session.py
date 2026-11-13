#!/usr/bin/env python3
"""Temporarily enable RX hardware stamps; restore configuration when stopped."""
import argparse
import ctypes
import grp
import json
import os
from pathlib import Path
import pwd
import signal
import socket
import stat
import time

class Config(ctypes.Structure):
    _fields_ = [('flags', ctypes.c_int), ('tx_type', ctypes.c_int), ('rx_filter', ctypes.c_int)]
class Data(ctypes.Union):
    _fields_ = [('pointer', ctypes.c_void_p), ('padding', ctypes.c_byte * 24)]
class Ifreq(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 16), ('data', Data)]
class TimestampInfo(ctypes.Structure):
    _fields_ = [('cmd', ctypes.c_uint32), ('so_timestamping', ctypes.c_uint32),
                ('phc_index', ctypes.c_int32), ('tx_types', ctypes.c_uint32),
                ('tx_reserved', ctypes.c_uint32 * 3), ('rx_filters', ctypes.c_uint32),
                ('rx_reserved', ctypes.c_uint32 * 3)]

def ioctl(interface, command, data):
    if not interface or len(interface.encode()) >= 16 or '/' in interface:
        raise ValueError('Invalid interface name')
    request = Ifreq()
    request.name = interface.encode()
    request.data.pointer = ctypes.addressof(data)
    libc = ctypes.CDLL(None, use_errno=True)
    libc.ioctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_void_p]
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        if libc.ioctl(sock.fileno(), command, ctypes.byref(request)) < 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error), interface)
    return data

def configuration(interface):
    c = ioctl(interface, 0x89b1, Config())  # SIOCGHWTSTAMP, read only.
    return {key: getattr(c, key) for key in ('flags', 'tx_type', 'rx_filter')}

def configure(interface, tx_type, rx_filter, flags=0):
    # A bond requires HWTSTAMP_FLAG_BONDED_PHC_INDEX (bit 0) as acknowledgement
    # that its active PHC can change. Physical ports normally use flags=0.
    c = ioctl(interface, 0x89b0, Config(flags, tx_type, rx_filter))  # SIOCSHWTSTAMP.
    return {key: getattr(c, key) for key in ('flags', 'tx_type', 'rx_filter')}

def phc_index(interface):
    return ioctl(interface, 0x8946, TimestampInfo(cmd=0x41)).phc_index  # ETHTOOL_GET_TS_INFO.

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--interface', required=True)
    parser.add_argument('--reader', required=True)
    parser.add_argument('--state', type=Path, required=True)
    parser.add_argument('--timeout-seconds', type=int, default=1800)
    parser.add_argument('--inspect', action='store_true')
    args = parser.parse_args()
    parent = Path('/sys/class/net') / args.interface
    slaves = parent / 'bonding/slaves'
    members = slaves.read_text().split() if slaves.exists() else []
    interfaces = [args.interface, *members]
    original = {name: configuration(name) for name in interfaces}
    devices = sorted({Path('/dev') / f'ptp{phc_index(name)}' for name in interfaces if phc_index(name) >= 0})
    if args.inspect:
        print(json.dumps({'configuration': original, 'phc_devices': list(map(str, devices))}, indent=2))
        return
    if os.geteuid() != 0:
        parser.error('Enabling device timestamping and PHC read access requires sudo')
    if args.timeout_seconds < 1:
        parser.error('Need a positive timeout')
    reader = pwd.getpwnam(args.reader)
    if any(name != args.reader for name in grp.getgrgid(reader.pw_gid).gr_mem):
        parser.error('Reader primary group has additional members; use a private group')
    stop_file = args.state.with_suffix(args.state.suffix + '.stop')
    if args.state.exists() or stop_file.exists():
        parser.error('State/stop file already exists; choose a fresh path')
    permissions = []
    for device in devices:
        s = device.stat()
        if not stat.S_ISCHR(s.st_mode):
            parser.error('PHC path is not a character device')
        permissions.append({'path': str(device), 'uid': s.st_uid, 'gid': s.st_gid, 'mode': stat.S_IMODE(s.st_mode)})
    state = {'interface': args.interface, 'reader': args.reader, 'original': original,
             'permissions': permissions, 'stop_file': str(stop_file), 'status': 'preparing'}
    if not args.state.parent.is_dir() or args.state.parent.stat().st_uid != reader.pw_uid:
        parser.error('Create the session directory as the reader first; it must allow the reader to create the stop file')
    with args.state.open('x') as stream:
        json.dump(state, stream, indent=2)
    os.chown(args.state, 0, reader.pw_gid)
    os.chmod(args.state, 0o640)
    def stopped(signum, frame):
        raise KeyboardInterrupt
    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, stopped)
    setup_error = None
    try:
        actual = configure(args.interface, 0, 1, original[args.interface]['flags'] & 1)
        if actual['rx_filter'] != 1:
            raise RuntimeError(f'Driver did not enable all-packet timestamps: {actual}')
        for item in permissions:
            os.chown(item['path'], item['uid'], reader.pw_gid)
            os.chmod(item['path'], item['mode'] | stat.S_IRGRP)
        state.update(status='ready', configured=actual)
        args.state.write_text(json.dumps(state, indent=2) + '\n')
        print(f"READY timestamping={args.interface} phc_readers_group={reader.pw_gid} stop_file={stop_file}", flush=True)
        deadline = time.monotonic() + args.timeout_seconds
        while time.monotonic() < deadline and not stop_file.exists():
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    except Exception as error:
        setup_error = str(error)
        raise
    finally:
        errors = []
        for name, config in original.items():
            try:
                configure(name, config['tx_type'], config['rx_filter'], config['flags'] & 1)
            except OSError as error:
                errors.append(str(error))
        for item in permissions:
            try:
                os.chown(item['path'], item['uid'], item['gid'])
                os.chmod(item['path'], item['mode'])
            except OSError as error:
                errors.append(str(error))
        state.update(status='restored' if not errors else 'restore_failed',
                     setup_error=setup_error, restore_errors=errors)
        args.state.write_text(json.dumps(state, indent=2) + '\n')
        print(json.dumps({'status': state['status'], 'errors': errors}), flush=True)
        if errors and setup_error is None:
            raise SystemExit(1)

if __name__ == '__main__':
    main()
